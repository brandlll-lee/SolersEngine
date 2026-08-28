/**************************************************************************/
/*  solers_tool_registry.cpp                                              */
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

#include "solers_tool_registry.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/run/editor_run_bar.h"
#include "editor/run/game_view_plugin.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "modules/solers_ai/plugins/solers_plugin.h"

void SolersToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_observation_service", "observation_service"), &SolersToolRegistry::set_observation_service);
	ClassDB::bind_method(D_METHOD("set_asset_service", "asset_service"), &SolersToolRegistry::set_asset_service);
	ClassDB::bind_method(D_METHOD("set_file_checkpoint", "file_checkpoint"), &SolersToolRegistry::set_file_checkpoint);
	ClassDB::bind_method(D_METHOD("set_reflection_service", "reflection_service"), &SolersToolRegistry::set_reflection_service);
	ClassDB::bind_method(D_METHOD("set_resource_service", "resource_service"), &SolersToolRegistry::set_resource_service);
	ClassDB::bind_method(D_METHOD("set_script_service", "script_service"), &SolersToolRegistry::set_script_service);
	ClassDB::bind_method(D_METHOD("set_permission_manager", "permission_manager"), &SolersToolRegistry::set_permission_manager);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersToolRegistry::set_action_timeline);
	ClassDB::bind_method(D_METHOD("register_default_tools"), &SolersToolRegistry::register_default_tools);
	ClassDB::bind_method(D_METHOD("list_tools"), &SolersToolRegistry::list_tools);
	ClassDB::bind_method(D_METHOD("get_skill_catalog_prompt"), &SolersToolRegistry::get_skill_catalog_prompt);
	ClassDB::bind_method(D_METHOD("get_model_tool_name", "name"), &SolersToolRegistry::get_model_tool_name);
	ClassDB::bind_method(D_METHOD("resolve_model_tool_name", "model_name"), &SolersToolRegistry::resolve_model_tool_name);
	ClassDB::bind_method(D_METHOD("call_tool", "name", "args"), &SolersToolRegistry::call_tool);
	ClassDB::bind_method(D_METHOD("get_tool_count"), &SolersToolRegistry::get_tool_count);
}

static const char *_exposure_name(SolersToolExposure p_exposure) {
	switch (p_exposure) {
		case SolersToolExposure::MODEL:
			return "model";
		case SolersToolExposure::OPERATION:
			return "operation";
		case SolersToolExposure::HIDDEN:
			return "hidden";
	}
	return "model";
}

static const char *_operation_domain_name(SolersOperationDomain p_domain) {
	switch (p_domain) {
		case SolersOperationDomain::EDITOR:
			return "editor";
		case SolersOperationDomain::RUNTIME:
			return "runtime";
		case SolersOperationDomain::PIPELINE:
			return "pipeline";
		case SolersOperationDomain::NONE:
			return "none";
	}
	return "none";
}

static const char *_operation_mode_name(SolersOperationMode p_mode) {
	return p_mode == SolersOperationMode::APPLY ? "apply" : "query";
}

static PackedStringArray _mutation_domain_names(SolersToolMutationDomain p_domains) {
	PackedStringArray names;
	if (solers_has_mutation_domain(p_domains, SolersToolMutationDomain::EDITOR)) {
		names.push_back("editor");
	}
	if (solers_has_mutation_domain(p_domains, SolersToolMutationDomain::FILES)) {
		names.push_back("files");
	}
	if (solers_has_mutation_domain(p_domains, SolersToolMutationDomain::IRREVERSIBLE)) {
		names.push_back("irreversible");
	}
	return names;
}

static bool _mutation_record_has_domain(const Dictionary &p_record, const String &p_domain) {
	return PackedStringArray(p_record.get("domains", PackedStringArray())).has(p_domain);
}

static const char *_ui_kind_name(SolersToolUiKind p_kind) {
	switch (p_kind) {
		case SolersToolUiKind::DEFAULT:
			return "default";
		case SolersToolUiKind::OBSERVE:
			return "observe";
		case SolersToolUiKind::READ:
			return "read";
		case SolersToolUiKind::SEARCH:
			return "search";
		case SolersToolUiKind::WRITE:
			return "write";
		case SolersToolUiKind::SCENE:
			return "scene";
		case SolersToolUiKind::SHELL:
			return "shell";
		case SolersToolUiKind::RUN:
			return "run";
		case SolersToolUiKind::NETWORK:
			return "network";
		case SolersToolUiKind::ASSET:
			return "asset";
		case SolersToolUiKind::CAPTURE:
			return "capture";
		case SolersToolUiKind::THINK:
			return "think";
		case SolersToolUiKind::SHIELD:
			return "shield";
	}
	return "default";
}

static UndoRedo *_current_scene_undo_redo(int &r_history_id) {
	r_history_id = EditorUndoRedoManager::INVALID_HISTORY;
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	if (!manager) {
		return nullptr;
	}
	r_history_id = EditorNode::get_singleton() && EditorNode::get_editor_data().get_edited_scene_count() > 0 ? EditorNode::get_editor_data().get_current_edited_scene_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY;
	if (r_history_id == EditorUndoRedoManager::INVALID_HISTORY) {
		return nullptr;
	}
	return manager->get_or_create_history(r_history_id).undo_redo;
}

Dictionary SolersToolRegistry::_ok(const Variant &p_data) const {
	Dictionary result;
	result["ok"] = true;
	result["data"] = p_data;
	return result;
}

Dictionary SolersToolRegistry::_error(const String &p_code, const String &p_message, bool p_recoverable) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	error["recoverable"] = p_recoverable;

	Dictionary result;
	result["ok"] = false;
	result["error"] = error;
	return result;
}

String SolersToolRegistry::_make_model_tool_name(const StringName &p_name) {
	const String name = String(p_name);
	String out;
	bool previous_was_separator = false;
	for (int i = 0; i < name.length(); i++) {
		const char32_t c = name[i];
		const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (allowed) {
			out += String::chr(c);
			previous_was_separator = false;
		} else if (!previous_was_separator) {
			out += "_";
			previous_was_separator = true;
		}
	}
	out = out.strip_edges();
	while (out.begins_with("_")) {
		out = out.substr(1);
	}
	while (out.ends_with("_")) {
		out = out.substr(0, out.length() - 1);
	}
	if (out.is_empty()) {
		return "tool";
	}
	return out;
}

Dictionary SolersToolRegistry::_schema(const char *p_json) {
	Dictionary generic;
	generic["type"] = "object";
	generic["properties"] = Dictionary();
	if (!p_json) {
		return generic;
	}
	const Variant parsed = JSON::parse_string(String::utf8(p_json));
	if (parsed.get_type() == Variant::DICTIONARY) {
		Dictionary schema = parsed;
		if (String(schema.get("type", String())) == "object" && schema.has("properties") && !schema.has("additionalProperties")) {
			schema["additionalProperties"] = false;
		}
		return schema;
	}
	ERR_PRINT("Solers tool registered with an invalid params schema; falling back to a generic object.");
	return generic;
}

static bool _tool_schema_type_matches(const Variant &p_value, const String &p_type) {
	if (p_type == "object") {
		switch (p_value.get_type()) {
			case Variant::DICTIONARY:
			case Variant::VECTOR2:
			case Variant::VECTOR2I:
			case Variant::RECT2:
			case Variant::RECT2I:
			case Variant::VECTOR3:
			case Variant::VECTOR3I:
			case Variant::TRANSFORM2D:
			case Variant::VECTOR4:
			case Variant::VECTOR4I:
			case Variant::PLANE:
			case Variant::QUATERNION:
			case Variant::AABB:
			case Variant::BASIS:
			case Variant::TRANSFORM3D:
			case Variant::COLOR:
				return true;
			default:
				return false;
		}
	}
	if (p_type == "array") {
		return p_value.get_type() == Variant::ARRAY;
	}
	if (p_type == "string") {
		return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME;
	}
	if (p_type == "integer") {
		if (p_value.get_type() == Variant::INT) {
			return true;
		}
		if (p_value.get_type() == Variant::FLOAT) {
			const double value = p_value;
			return Math::is_finite(value) && value == Math::floor(value);
		}
		return false;
	}
	if (p_type == "number") {
		return p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT;
	}
	if (p_type == "boolean") {
		return p_value.get_type() == Variant::BOOL;
	}
	return true;
}

static Dictionary _tool_result_envelope(const Dictionary &p_result, const String &p_call_id) {
	Dictionary result = p_result.duplicate(true);
	const Variant ok_value = result.get("ok", Variant());
	if (ok_value.get_type() != Variant::BOOL) {
		Dictionary error;
		error["code"] = "TOOL_RESULT_INVALID";
		error["message"] = "The tool handler returned an invalid result envelope.";
		error["recoverable"] = false;
		result.clear();
		result["ok"] = false;
		result["error"] = error;
	} else if ((bool)ok_value) {
		if (!result.has("data")) {
			result["data"] = Dictionary();
		}
	} else {
		Dictionary error = result.get("error", Dictionary());
		if (String(error.get("code", String())).is_empty() || String(error.get("message", String())).is_empty()) {
			error.clear();
			error["code"] = "TOOL_RESULT_INVALID";
			error["message"] = "The tool handler returned an invalid error envelope.";
			error["recoverable"] = false;
		} else if (!error.has("recoverable")) {
			error["recoverable"] = true;
		}
		result["error"] = error;
	}
	if (!p_call_id.is_empty()) {
		result["call_id"] = p_call_id;
	}
	return result;
}

static bool _validate_tool_schema_value(const Variant &p_value, const Dictionary &p_schema, const String &p_path, String &r_error) {
	const Variant const_value = p_schema.get("const", Variant());
	const bool comparable_strings = (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) &&
			(const_value.get_type() == Variant::STRING || const_value.get_type() == Variant::STRING_NAME);
	if (p_schema.has("const") && (comparable_strings ? String(p_value) != String(const_value) : p_value != const_value)) {
		r_error = vformat("%s must equal %s.", p_path, JSON::stringify(p_schema["const"]));
		return false;
	}
	const Array one_of = p_schema.get("oneOf", Array());
	if (!one_of.is_empty()) {
		int matches = 0;
		String first_error;
		for (int i = 0; i < one_of.size(); i++) {
			if (one_of[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			String branch_error;
			if (_validate_tool_schema_value(p_value, one_of[i], p_path, branch_error)) {
				matches++;
			} else if (first_error.is_empty()) {
				first_error = branch_error;
			}
		}
		if (matches != 1) {
			r_error = matches == 0 ? first_error : vformat("%s matches more than one schema branch.", p_path);
			return false;
		}
		return true;
	}
	const Array any_of = p_schema.get("anyOf", Array());
	if (!any_of.is_empty()) {
		String first_error;
		for (int i = 0; i < any_of.size(); i++) {
			if (any_of[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			String branch_error;
			if (_validate_tool_schema_value(p_value, any_of[i], p_path, branch_error)) {
				return true;
			}
			if (first_error.is_empty()) {
				first_error = branch_error;
			}
		}
		r_error = first_error.is_empty() ? vformat("%s does not match any supported schema.", p_path) : first_error;
		return false;
	}

	const String type = p_schema.get("type", String());
	if (!type.is_empty() && !_tool_schema_type_matches(p_value, type)) {
		r_error = vformat("%s must be %s.", p_path, type);
		return false;
	}
	const Array allowed = p_schema.get("enum", Array());
	if (!allowed.is_empty() && !allowed.has(p_value)) {
		r_error = vformat("%s must be one of %s.", p_path, JSON::stringify(allowed));
		return false;
	}
	if (type == "number" || type == "integer") {
		const double value = p_value;
		if (!Math::is_finite(value)) {
			r_error = vformat("%s must be finite.", p_path);
			return false;
		}
		if (p_schema.has("minimum") && value < (double)p_schema["minimum"]) {
			r_error = vformat("%s must be at least %s.", p_path, p_schema["minimum"]);
			return false;
		}
		if (p_schema.has("maximum") && value > (double)p_schema["maximum"]) {
			r_error = vformat("%s must be at most %s.", p_path, p_schema["maximum"]);
			return false;
		}
		if (p_schema.has("exclusiveMinimum") && value <= (double)p_schema["exclusiveMinimum"]) {
			r_error = vformat("%s must be greater than %s.", p_path, p_schema["exclusiveMinimum"]);
			return false;
		}
		if (p_schema.has("exclusiveMaximum") && value >= (double)p_schema["exclusiveMaximum"]) {
			r_error = vformat("%s must be less than %s.", p_path, p_schema["exclusiveMaximum"]);
			return false;
		}
	}
	if (type == "string" && p_schema.has("minLength") && String(p_value).length() < (int)p_schema["minLength"]) {
		r_error = vformat("%s is too short.", p_path);
		return false;
	}
	if (type == "array") {
		const Array values = p_value;
		if (p_schema.has("minItems") && values.size() < (int)p_schema["minItems"]) {
			r_error = vformat("%s requires at least %d item(s).", p_path, (int)p_schema["minItems"]);
			return false;
		}
		if (p_schema.has("maxItems") && values.size() > (int)p_schema["maxItems"]) {
			r_error = vformat("%s allows at most %d item(s).", p_path, (int)p_schema["maxItems"]);
			return false;
		}
		const Dictionary items = p_schema.get("items", Dictionary());
		for (int i = 0; i < values.size() && !items.is_empty(); i++) {
			if (!_validate_tool_schema_value(values[i], items, vformat("%s[%d]", p_path, i), r_error)) {
				return false;
			}
		}
	}
	if (type == "object") {
		const Dictionary properties = p_schema.get("properties", Dictionary());
		const Array required = p_schema.get("required", Array());
		Dictionary value;
		if (p_value.get_type() == Variant::DICTIONARY) {
			value = p_value;
		} else {
			for (const Variant &key : properties.keys()) {
				bool valid = false;
				const Variant component = p_value.get_named(StringName(key), valid);
				if (!valid) {
					r_error = vformat("%s must be an object with named properties.", p_path);
					return false;
				}
				value[key] = component;
			}
		}
		for (const Variant &key : required) {
			if (!value.has(key)) {
				r_error = vformat("%s.%s is required.", p_path, key);
				return false;
			}
		}
		const bool additional = p_schema.get("additionalProperties", true);
		const Variant *key = nullptr;
		while ((key = value.next(key))) {
			if (!properties.has(*key)) {
				if (!additional) {
					r_error = vformat("%s.%s is not supported.", p_path, *key);
					return false;
				}
				continue;
			}
			if (!_validate_tool_schema_value(value[*key], properties[*key], vformat("%s.%s", p_path, *key), r_error)) {
				return false;
			}
		}
	}
	return true;
}

#include "modules/solers_ai/core/solers_builtin_skills.h"
static String _trace_json(const Variant &p_value, int p_max_chars) {
	String text = JSON::stringify(p_value, "", false, true).replace("\n", " ");
	return text.length() > p_max_chars ? text.substr(0, p_max_chars) + "..." : text;
}

static Variant _trace_value_shape(const Variant &p_value) {
	Dictionary out;
	out["type"] = Variant::get_type_name(p_value.get_type());
	if (p_value.get_type() == Variant::STRING) {
		out["bytes"] = String(p_value).utf8().length();
	} else if (p_value.get_type() == Variant::ARRAY) {
		out["count"] = ((Array)p_value).size();
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		out["count"] = ((Dictionary)p_value).size();
	}
	return out;
}

static Variant _trace_arg_value(const Variant &p_value) {
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary in = p_value;
		Dictionary out;
		const Variant *key = nullptr;
		while ((key = in.next(key))) {
			const String key_name = String(*key);
			const Variant value = in[*key];
			out[*key] = key_name == "value" ? _trace_value_shape(value) : _trace_arg_value(value);
		}
		return out;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array in = p_value;
		Array items;
		const int cap = MIN(in.size(), 32);
		for (int i = 0; i < cap; i++) {
			items.push_back(_trace_arg_value(in[i]));
		}
		Dictionary out;
		out["count"] = in.size();
		out["items"] = items;
		if (in.size() > cap) {
			out["truncated"] = true;
		}
		return out;
	}
	if (p_value.get_type() == Variant::STRING && String(p_value).utf8().length() > 80) {
		return vformat("<string %d bytes>", String(p_value).utf8().length());
	}
	return p_value;
}

static bool _cap_redacts_key(const SolersToolCapability &p_cap, const String &p_key) {
	for (int i = 0; i < p_cap.redact_args.size(); i++) {
		if (p_cap.redact_args[i] == p_key) {
			return true;
		}
	}
	return false;
}

static Dictionary _trace_args(const Dictionary &p_args, const SolersToolCapability *p_cap = nullptr) {
	Dictionary out;
	const Variant *key = nullptr;
	while ((key = p_args.next(key))) {
		const String key_name = String(*key);
		const Variant value = p_args[*key];
		if (p_cap && _cap_redacts_key(*p_cap, key_name) && value.get_type() != Variant::ARRAY && value.get_type() != Variant::DICTIONARY) {
			out[*key] = _trace_value_shape(value);
		} else {
			out[*key] = _trace_arg_value(value);
		}
	}
	return out;
}

static Dictionary _solers_scene_state_receipt() {
	Dictionary receipt;
	EditorNode *editor = EditorNode::get_singleton();
	if (!editor || EditorNode::get_editor_data().get_edited_scene_count() == 0) {
		return receipt;
	}
	Node *root = editor->get_edited_scene();
	const String path = root ? root->get_scene_file_path() : String();
	const int history_id = EditorNode::get_editor_data().get_current_edited_scene_history_id();
	receipt["has_root"] = root != nullptr;
	if (root) {
		receipt["root_object_id"] = solers_object_id_to_string(root->get_instance_id());
	}
	receipt["scene_path"] = path;
	receipt["history_id"] = history_id;
	if (EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton()) {
		if (history_id != EditorUndoRedoManager::INVALID_HISTORY) {
			receipt["version"] = (int64_t)manager->get_or_create_history(history_id).undo_redo->get_version();
		}
	}
	if (!path.is_empty()) {
		const ResourceUID::ID uid = ResourceLoader::get_resource_uid(path);
		if (uid != ResourceUID::INVALID_ID) {
			receipt["resource_uid"] = ResourceUID::get_singleton()->id_to_text(uid);
		}
		if (FileAccess::exists(path)) {
			receipt["saved_sha256"] = FileAccess::get_sha256(path);
		}
	}
	return receipt;
}

static Dictionary _solers_checkpoint_target_state(const SolersFileCheckpoint *p_service, const String &p_path) {
	return p_service ? Dictionary(p_service->get_path_state(p_path).get("data", Dictionary())) : Dictionary();
}

static Dictionary _solers_resource_state_receipt(const SolersFileCheckpoint *p_service, const String &p_path) {
	const Dictionary state = _solers_checkpoint_target_state(p_service, p_path);
	Dictionary receipt;
	receipt["path"] = p_path;
	const bool exists = state.get("existed", false);
	receipt["exists"] = exists;
	if (exists) {
		receipt["sha256"] = state.get("content_sha256", String());
		receipt["directory"] = state.get("directory", false);
	}
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_path);
	if (uid != ResourceUID::INVALID_ID) {
		receipt["resource_uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	return receipt;
}

static bool _solers_checkpoint_matches(const SolersFileCheckpoint *p_service, const Dictionary &p_checkpoint, bool p_after) {
	const Dictionary state = _solers_checkpoint_target_state(p_service, p_checkpoint.get("path", String()));
	const bool expected_exists = p_checkpoint.get(p_after ? "exists_after" : "existed", false);
	const String expected_sha = p_checkpoint.get(p_after ? "sha256_after" : "content_sha256", String());
	return (bool)state.get("existed", false) == expected_exists && (!expected_exists || String(state.get("content_sha256", String())) == expected_sha);
}

static Array _solers_operation_targets(const Dictionary &p_data) {
	Array targets;
	Dictionary target;
	for (const char *field : { "node_path", "object_id", "class_name", "native_facts", "path", "sha256" }) {
		if (p_data.has(field)) {
			target[field] = p_data[field];
		}
	}
	if (!target.is_empty()) {
		targets.push_back(target);
	}
	return targets;
}

static bool _solers_scene_state_matches(const Dictionary &p_expected, const Dictionary &p_actual) {
	if ((int64_t)p_expected.get("history_id", -1) != (int64_t)p_actual.get("history_id", -2) ||
			(int64_t)p_expected.get("version", -1) != (int64_t)p_actual.get("version", -2)) {
		return false;
	}
	return !p_expected.has("root_object_id") || p_expected.get("root_object_id", String()) == p_actual.get("root_object_id", String());
}

static std::function<Array(const Dictionary &)> _access_by_arg(const char *p_mode, const char *p_prefix, const char *p_arg) {
	const String mode = String::utf8(p_mode);
	const String prefix = String::utf8(p_prefix);
	const String arg = String::utf8(p_arg);
	return [mode, prefix, arg](const Dictionary &p_args) {
		Array accesses;
		Dictionary access;
		access["mode"] = mode;
		const String value = String(p_args.get(arg, String())).strip_edges();
		access["key"] = value.is_empty() ? String("*") : prefix + value;
		accesses.push_back(access);
		return accesses;
	};
}

static String _trace_result(const Dictionary &p_result) {
	const bool ok = p_result.get("ok", false);
	String out = vformat("ok=%d", (int)ok);
	if (!ok) {
		const Dictionary error = p_result.get("error", Dictionary());
		out += vformat(" error=%s", String(error.get("code", error.get("message", String()))));
		const String message = error.get("message", String());
		if (!message.is_empty()) {
			out += vformat(" message=%s", message);
		}
		const Dictionary validation = error.get("validation", Dictionary());
		const Array errors = validation.get("errors", Array());
		if (!errors.is_empty() && errors[0].get_type() == Variant::DICTIONARY) {
			const Dictionary first = errors[0];
			out += vformat(" line=%d column=%d detail=%s", (int)first.get("line", 0), (int)first.get("column", 0), String(first.get("message", String())));
		}
	}
	const Variant data_value = p_result.get("data", Variant());
	if (data_value.get_type() == Variant::DICTIONARY) {
		const Dictionary data = data_value;
		if (data.has("count")) {
			out += vformat(" count=%d", (int)data.get("count", 0));
		}
		if (data.has("content")) {
			out += vformat(" bytes=%d", String(data.get("content", String())).utf8().length());
		}
		if (data.has("file_index")) {
			const Dictionary file_index = data.get("file_index", Dictionary());
			out += vformat(" file_count=%d", (int)file_index.get("count", 0));
		}
	} else if (ok && data_value.get_type() == Variant::ARRAY) {
		out += vformat(" count=%d", ((Array)data_value).size());
	}
	return out.length() > 240 ? out.substr(0, 240) + "..." : out;
}

static bool _schema_requires_key(const Dictionary &p_schema, const Variant &p_key) {
	const Array required = p_schema.get("required", Array());
	const String key = String(p_key);
	for (int i = 0; i < required.size(); i++) {
		if (String(required[i]) == key) {
			return true;
		}
	}
	return false;
}

static Dictionary _schema_for_key(const Dictionary &p_schema, const Variant &p_key) {
	const Dictionary properties = p_schema.get("properties", Dictionary());
	const Variant value = properties.get(p_key, Variant());
	if (value.get_type() != Variant::DICTIONARY) {
		return Dictionary();
	}
	return value;
}

static Variant _normalize_tool_arg_value(const Variant &p_value, const Dictionary &p_schema) {
	const Array one_of = p_schema.get("oneOf", Array());
	if (!one_of.is_empty()) {
		Variant match;
		int matches = 0;
		for (int i = 0; i < one_of.size(); i++) {
			if (one_of[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary branch = one_of[i];
			const Variant candidate = _normalize_tool_arg_value(p_value, branch);
			String error;
			if (_validate_tool_schema_value(candidate, branch, "parameters", error)) {
				match = candidate;
				matches++;
			}
		}
		if (matches == 1) {
			return match;
		}
	}
	if (String(p_schema.get("type", String())) == "integer" && p_value.get_type() == Variant::FLOAT) {
		const double value = p_value;
		if (Math::is_finite(value) && Math::floor(value) == value && value >= (double)INT64_MIN && value < (double)INT64_MAX) {
			return (int64_t)value;
		}
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary in = p_value;
		Dictionary out;
		const Dictionary properties = p_schema.get("properties", Dictionary());
		const Variant *key = nullptr;
		while ((key = in.next(key))) {
			const Variant value = in[*key];
			const bool schema_declares_key = properties.has(*key);
			if (schema_declares_key && value.get_type() == Variant::STRING && String(value).is_empty() && !_schema_requires_key(p_schema, *key)) {
				continue;
			}
			out[*key] = _normalize_tool_arg_value(value, _schema_for_key(p_schema, *key));
		}
		return out;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array in = p_value;
		Array out;
		const Variant items = p_schema.get("items", Dictionary());
		Dictionary item_schema;
		if (items.get_type() == Variant::DICTIONARY) {
			item_schema = items;
		}
		for (int i = 0; i < in.size(); i++) {
			out.push_back(_normalize_tool_arg_value(in[i], item_schema));
		}
		return out;
	}
	return p_value;
}

static Dictionary _normalize_tool_args(const Dictionary &p_args, const Dictionary &p_schema) {
	const Variant normalized = _normalize_tool_arg_value(p_args, p_schema);
	if (normalized.get_type() != Variant::DICTIONARY) {
		return p_args;
	}
	return normalized;
}

void SolersToolRegistry::_clear_tools() {
	for (KeyValue<StringName, SolersTool *> &E : tools) {
		memdelete(E.value);
	}
	tools.clear();
	model_name_index.clear();
	tool_catalog.clear();
	tool_catalog_by_name.clear();
	delivered_addon_contracts.clear();
}

void SolersToolRegistry::_register(SolersTool *p_tool) {
	const StringName name = p_tool->name();
	const Dictionary schema = p_tool->parameters_schema();
	const Variant root_properties = schema.get("properties", Variant());
	if (String(schema.get("type", String())) != "object" || root_properties.get_type() != Variant::DICTIONARY ||
			schema.has("oneOf") || schema.has("anyOf") || schema.has("allOf")) {
		ERR_PRINT(vformat("Solers tool '%s' must declare one portable object-root input schema.", name));
		memdelete(p_tool);
		return;
	}
	const SolersToolMutationDomain mutation_domains = p_tool->capability().mutation_domains;
	if (solers_has_mutation_domain(mutation_domains, SolersToolMutationDomain::IRREVERSIBLE) && mutation_domains != SolersToolMutationDomain::IRREVERSIBLE) {
		ERR_PRINT(vformat("Irreversible Solers tool '%s' cannot declare reversible mutation domains.", name));
		memdelete(p_tool);
		return;
	}
	const SolersToolCapability &capability = p_tool->capability();
	if (p_tool->exposure() == SolersToolExposure::OPERATION && capability.operation_domain == SolersOperationDomain::NONE) {
		ERR_PRINT(vformat("Solers operation '%s' must declare an authority domain.", name));
		memdelete(p_tool);
		return;
	}
	if ((solers_has_mutation_domain(mutation_domains, SolersToolMutationDomain::EDITOR) || solers_has_mutation_domain(mutation_domains, SolersToolMutationDomain::FILES)) && p_tool->capability().execution == SolersToolExecution::WORKER_THREAD) {
		ERR_PRINT(vformat("Reversible Solers tool '%s' must execute on the main thread.", name));
		memdelete(p_tool);
		return;
	}
	if (tools.has(name)) {
		ERR_PRINT(vformat("Solers tool already registered: %s", name));
		memdelete(p_tool);
		return;
	}
	if (p_tool->exposure() == SolersToolExposure::MODEL) {
		const StringName model_name = StringName(_make_model_tool_name(name));
		if (model_name_index.has(model_name) && model_name_index[model_name] != name) {
			ERR_PRINT(vformat("Solers model tool name collision: %s maps to both %s and %s.", String(model_name), String(model_name_index[model_name]), String(name)));
			memdelete(p_tool);
			return;
		}
		model_name_index[model_name] = name;
	}
	tools[name] = p_tool;
}

void SolersToolRegistry::_add(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersPermissionManager::Permission p_permission, SolersToolMutationDomain p_mutation_domains,
		const Vector<String> &p_redact,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
		SolersToolExecution p_execution, std::function<Array(const Dictionary &)> p_resource_access,
		SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler, SolersFunctionTool::CompletionHandler p_completion_handler,
		std::function<SolersPermissionManager::Permission(const Dictionary &)> p_permission_resolver,
		std::function<SolersToolMutationDomain(const Dictionary &)> p_mutation_domain_resolver,
		SolersToolUiKind p_ui_kind, const SolersToolHostPolicy &p_host, const StringName &p_target_class,
		SolersOperationDomain p_operation_domain, SolersOperationMode p_operation_mode,
		std::function<SolersToolExecution(const Dictionary &)> p_execution_resolver, const StringName &p_target_kind,
		std::function<bool(const Dictionary &)> p_execution_ready) {
	SolersToolCapability cap;
	cap.permission = p_permission;
	cap.permission_resolver = std::move(p_permission_resolver);
	cap.mutation_domains = p_mutation_domains;
	cap.mutation_domain_resolver = std::move(p_mutation_domain_resolver);
	cap.execution = p_execution;
	cap.execution_resolver = std::move(p_execution_resolver);
	cap.execution_ready = std::move(p_execution_ready);
	cap.resource_access = std::move(p_resource_access);
	cap.redact_args = p_redact;
	cap.ui_kind = p_ui_kind;
	cap.host = p_host;
	cap.target_class = p_target_class;
	cap.operation_domain = p_operation_domain;
	cap.operation_mode = p_operation_mode;
	cap.target_kind = p_target_kind;
	SolersTool *tool = memnew(SolersFunctionTool(StringName(String::utf8(p_name)), String::utf8(p_description),
			_schema(p_schema_json), p_exposure, cap, std::move(p_handler), std::move(p_poll_handler), std::move(p_ready_handler), std::move(p_completion_handler)));
	_register(tool);
}

void SolersToolRegistry::_add_observe_exposed(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
		std::function<Array(const Dictionary &)> p_resource_access, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler,
		SolersToolUiKind p_ui_kind, SolersToolExecution p_execution, const SolersToolHostPolicy &p_host,
		SolersOperationDomain p_operation_domain, SolersOperationMode p_operation_mode) {
	_add(p_name, p_description, p_schema_json, SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationDomain::NONE,
			Vector<String>(), p_exposure, std::move(p_handler),
			p_execution, std::move(p_resource_access), std::move(p_poll_handler), std::move(p_ready_handler), {}, {}, {}, p_ui_kind, p_host, StringName(), p_operation_domain, p_operation_mode);
}

void SolersToolRegistry::_add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler,
		SolersToolUiKind p_ui_kind) {
	_add_observe_exposed(p_name, p_description, p_schema_json, SolersToolExposure::MODEL, std::move(p_handler), std::move(p_resource_access), std::move(p_poll_handler), std::move(p_ready_handler), p_ui_kind);
}

void SolersToolRegistry::_add_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode,
		const char *p_name, const char *p_description, const char *p_schema_json,
		SolersPermissionManager::Permission p_permission, SolersToolMutationDomain p_mutation_domains,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access,
		const StringName &p_target_class,
		SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler, SolersFunctionTool::CompletionHandler p_completion_handler) {
	_add(p_name, p_description, p_schema_json, p_permission, p_mutation_domains, {}, SolersToolExposure::OPERATION,
			std::move(p_handler), SolersToolExecution::MAIN_THREAD, std::move(p_resource_access), std::move(p_poll_handler), std::move(p_ready_handler), std::move(p_completion_handler), {}, {}, SolersToolUiKind::DEFAULT, {}, p_target_class, p_domain, p_mode);
}

void SolersToolRegistry::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
}

void SolersToolRegistry::set_asset_service(SolersAssetService *p_asset_service) {
	asset_service = p_asset_service;
}

void SolersToolRegistry::set_file_checkpoint(SolersFileCheckpoint *p_file_checkpoint) {
	file_checkpoint = p_file_checkpoint;
}

void SolersToolRegistry::set_reflection_service(SolersReflectionService *p_reflection_service) {
	reflection_service = p_reflection_service;
}

void SolersToolRegistry::set_resource_service(SolersResourceService *p_resource_service) {
	resource_service = p_resource_service;
}

void SolersToolRegistry::set_script_service(SolersScriptService *p_script_service) {
	script_service = p_script_service;
}

void SolersToolRegistry::set_permission_manager(SolersPermissionManager *p_permission_manager) {
	permission_manager = p_permission_manager;
}

void SolersToolRegistry::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

Dictionary SolersToolRegistry::_prepare_reversal(SolersPreparedToolCall &r_call) {
	if (r_call.mutation_domains == SolersToolMutationDomain::NONE) {
		return Dictionary();
	}

	Dictionary state;
	state["domains"] = _mutation_domain_names(r_call.mutation_domains);
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::IRREVERSIBLE)) {
		state["scene_state_before"] = _solers_scene_state_receipt();
		r_call.reversal_state = state;
		return Dictionary();
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		int history_id = EditorUndoRedoManager::INVALID_HISTORY;
		UndoRedo *undo_redo = _current_scene_undo_redo(history_id);
		if (!undo_redo) {
			return _tool_result_envelope(_error("UNDO_HISTORY_UNAVAILABLE", "The current edited scene has no UndoRedo history.", true), r_call.context.call_id);
		}
		state["history_id"] = history_id;
		state["version_before"] = (int64_t)undo_redo->get_version();
		state["scene_state_before"] = _solers_scene_state_receipt();
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		if (!file_checkpoint) {
			return _tool_result_envelope(_error("CHECKPOINT_SERVICE_UNAVAILABLE", "The file checkpoint service is not initialized.", false), r_call.context.call_id);
		}
		Array checkpoints;
		Array resource_states_before;
		HashSet<String> seen_paths;
		const Array accesses = resolve_resource_access(r_call.name, r_call.args);
		for (int i = 0; i < accesses.size(); i++) {
			const Dictionary access = accesses[i];
			const String key = access.get("key", String());
			if (String(access.get("mode", "write")) != "write" || !key.begins_with("project:res://")) {
				continue;
			}
			const String path = key.trim_prefix("project:");
			if (seen_paths.has(path)) {
				continue;
			}
			seen_paths.insert(path);
			const Dictionary checkpoint = file_checkpoint->create_checkpoint(path, vformat("Solers tool %s", r_call.name));
			if (!(bool)checkpoint.get("ok", false)) {
				for (const Variant &prepared : checkpoints) {
					file_checkpoint->discard_checkpoint_state(prepared);
				}
				return _tool_result_envelope(checkpoint, r_call.context.call_id);
			}
			checkpoints.push_back(checkpoint.get("data", Dictionary()));
			resource_states_before.push_back(_solers_resource_state_receipt(file_checkpoint, path));
		}
		if (checkpoints.is_empty()) {
			return _tool_result_envelope(_error("CHECKPOINT_TARGET_UNDECLARED", vformat("Tool '%s' must declare concrete project file write targets.", r_call.name), false), r_call.context.call_id);
		}
		state["checkpoints"] = checkpoints;
		state["resource_states_before"] = resource_states_before;
	}
	r_call.reversal_state = state;
	return Dictionary();
}

void SolersToolRegistry::_discard_reversal(const Dictionary &p_record) {
	if (!file_checkpoint) {
		return;
	}
	const Array checkpoints = p_record.get("checkpoints", Array());
	for (int i = 0; i < checkpoints.size(); i++) {
		file_checkpoint->discard_checkpoint_state(checkpoints[i]);
	}
}

Dictionary SolersToolRegistry::_finalize_prepared_result(SolersPreparedToolCall &r_call, const Dictionary &p_result) {
	Dictionary result = p_result.duplicate(true);
	const Dictionary pending_data = result.get("data", Dictionary());
	if ((bool)result.get("ok", false) && String(pending_data.get("status", String())) == "pending") {
		return result;
	}
	if (r_call.mutation_domains == SolersToolMutationDomain::NONE) {
		return result;
	}

	auto rollback = [&]() -> bool {
		bool restored = true;
		if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
			const int history_id = r_call.reversal_state.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const uint64_t version_before = (int64_t)r_call.reversal_state.get("version_before", 0);
			EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			while (manager && undo_redo && undo_redo->get_version() > version_before && manager->undo_history(history_id)) {
			}
			restored = restored && undo_redo && undo_redo->get_version() == version_before;
		}
		if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
			const Array checkpoints = r_call.reversal_state.get("checkpoints", Array());
			for (int i = checkpoints.size() - 1; i >= 0; i--) {
				const Dictionary checkpoint = checkpoints[i];
				if (!_solers_checkpoint_matches(file_checkpoint, checkpoint, false) && (!file_checkpoint || !(bool)file_checkpoint->restore_checkpoint_state(checkpoint).get("ok", false))) {
					restored = false;
				}
			}
			_discard_reversal(r_call.reversal_state);
		}
		return restored;
	};

	if (!(bool)result.get("ok", false)) {
		if (!rollback()) {
			Dictionary rollback_error = _error("TOOL_ROLLBACK_FAILED", vformat("Tool '%s' failed and its previous state could not be restored.", r_call.name), false);
			Dictionary error = rollback_error.get("error", Dictionary());
			error["original_error"] = result.get("error", Dictionary());
			rollback_error["error"] = error;
			return _tool_result_envelope(rollback_error, r_call.context.call_id);
		}
		return result;
	}
	Dictionary data = result.get("data", Dictionary());
	const bool checkpoint_consumed = data.get("checkpoint_consumed", false);
	if (checkpoint_consumed) {
		r_call.journal_event["event_type"] = "checkpoint_consumed";
		r_call.journal_event["reversal_id"] = data.get("reversal_id", String());
	}
	bool changed = solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::IRREVERSIBLE) && (bool)data.get("authored_state_changed", false);
	bool editor_changed = false;
	bool files_changed = false;
	Dictionary record = r_call.reversal_state.duplicate(true);
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		const uint64_t version_before = (int64_t)record.get("version_before", 0);
		EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
		const uint64_t version_after = undo_redo ? undo_redo->get_version() : version_before;
		editor_changed = version_after != version_before;
		changed = changed || editor_changed;
		if (version_after != version_before + 1) {
			Dictionary failure = _error("TOOL_UNDO_CONTRACT_VIOLATION", vformat("Tool '%s' must commit exactly one UndoRedo action.", r_call.name), false);
			failure["data"] = Dictionary({ { "history_id", history_id }, { "version_before", (int64_t)version_before }, { "version_after", (int64_t)version_after }, { "scene_before", record.get("scene_state_before", Dictionary()) }, { "scene_after", _solers_scene_state_receipt() } });
			rollback();
			return _tool_result_envelope(failure, r_call.context.call_id);
		}
		record["version_after"] = (int64_t)version_after;
	}
	if (editor_changed && affects_scene_state(r_call.name, r_call.args)) {
		EditorInterface *editor = EditorInterface::get_singleton();
		Node *root = editor ? editor->get_edited_scene_root() : nullptr;
		String scene_path = root ? root->get_scene_file_path() : String();
		if (scene_path.is_empty()) {
			scene_path = String(r_call.args.get("save_path", String())).strip_edges();
		}
		if (!root || !scene_path.begins_with("res://") || (scene_path.get_extension().to_lower() != "tscn" && scene_path.get_extension().to_lower() != "scn")) {
			rollback();
			return _tool_result_envelope(_error("SCENE_PATH_REQUIRED", "An unsaved scene transaction requires an explicit res://*.tscn or res://*.scn save_path.", false), r_call.context.call_id);
		}
		if (root->get_scene_file_path().is_empty()) {
			editor->save_scene_as(scene_path);
		} else {
			editor->save_scene();
		}
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
		const bool persisted = FileAccess::exists(scene_path) && root->get_scene_file_path() == scene_path && manager && !manager->is_history_unsaved(history_id);
		if (!persisted) {
			rollback();
			return _tool_result_envelope(_error("SCENE_SAVE_FAILED", "Godot did not confirm the scene path, disk file, and UndoRedo saved version; the action was rolled back.", false), r_call.context.call_id);
		}
		data["persisted"] = true;
		data["path"] = scene_path;
		data["saved_sha256"] = FileAccess::get_sha256(scene_path);
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		Array checkpoints = record.get("checkpoints", Array());
		for (int i = 0; i < checkpoints.size(); i++) {
			Dictionary checkpoint = checkpoints[i];
			const Dictionary state_after = _solers_checkpoint_target_state(file_checkpoint, checkpoint.get("path", String()));
			const bool exists_after = state_after.get("existed", false);
			const String sha_after = state_after.get("content_sha256", String());
			checkpoint["exists_after"] = exists_after;
			checkpoint["sha256_after"] = sha_after;
			Dictionary settings_after;
			const Dictionary settings_before = checkpoint.get("project_settings", Dictionary());
			for (const Variant *setting = settings_before.next(nullptr); setting; setting = settings_before.next(setting)) {
				settings_after[*setting] = ProjectSettings::get_singleton()->get(*setting);
			}
			if (!settings_after.is_empty()) {
				checkpoint["project_settings_after"] = settings_after;
			}
			files_changed = files_changed || exists_after != (bool)checkpoint.get("existed", false) || (exists_after && sha_after != String(checkpoint.get("content_sha256", String())));
			checkpoints[i] = checkpoint;
		}
		record["checkpoints"] = checkpoints;
		changed = changed || files_changed;
	}
	if (!changed) {
		_discard_reversal(record);
		data["authored_state_changed"] = false;
		result["data"] = data;
		return result;
	}

	Dictionary receipt;
	receipt["call_id"] = r_call.context.call_id;
	receipt["domains"] = _mutation_domain_names(r_call.mutation_domains);
	const Array targets = _solers_operation_targets(data);
	if (!targets.is_empty()) {
		receipt["targets"] = targets;
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		receipt["scene_before"] = r_call.reversal_state.get("scene_state_before", Dictionary());
		receipt["scene_after"] = _solers_scene_state_receipt();
		data.erase("results");
		data.erase("state_before");
		data.erase("state_after");
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		receipt["resources_before"] = r_call.reversal_state.get("resource_states_before", Array());
		Array resources_after;
		const Array checkpoints = record.get("checkpoints", Array());
		for (int i = 0; i < checkpoints.size(); i++) {
			resources_after.push_back(_solers_resource_state_receipt(file_checkpoint, Dictionary(checkpoints[i]).get("path", String())));
		}
		receipt["resources_after"] = resources_after;
	}
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::IRREVERSIBLE)) {
		receipt["scene_before"] = r_call.reversal_state.get("scene_state_before", Dictionary());
		receipt["scene_after"] = _solers_scene_state_receipt();
	}
	record["receipt"] = receipt;

	data["authored_state_changed"] = true;
	Dictionary mutation;
	mutation["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
	mutation["domains"] = _mutation_domain_names(r_call.mutation_domains);
	mutation["receipt"] = receipt;
	const String session_key = r_call.context.session_id.is_empty() ? String("direct") : r_call.context.session_id;
	if (solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::EDITOR) || solers_has_mutation_domain(r_call.mutation_domains, SolersToolMutationDomain::FILES)) {
		Vector<Dictionary> &stack = reversals_by_session[session_key];
		const String reversal_id = (session_key + ":" + r_call.context.call_id + ":" + String::num_uint64(r_call.context.authored_revision + 1) + ":" + String::num_int64(stack.size() + 1)).sha256_text();
		record["id"] = reversal_id;
		record["session_id"] = session_key;
		record["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
		stack.push_back(record);
		mutation["reversal_id"] = reversal_id;
		r_call.journal_event["event_type"] = "checkpoint_created";
		r_call.journal_event["checkpoint"] = record;
		r_call.journal_event["note"] = "Protective checkpoint for history.revert; not a rollback of your edit.";
	} else if (!checkpoint_consumed) {
		Vector<Dictionary> &stack = reversals_by_session[session_key];
		for (const Dictionary &previous : stack) {
			_discard_reversal(previous);
		}
		stack.clear();
		Dictionary barrier = record;
		barrier["id"] = (session_key + ":barrier:" + String::num_uint64(r_call.context.authored_revision + 1)).sha256_text();
		barrier["session_id"] = session_key;
		barrier["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
		stack.push_back(barrier);
		r_call.journal_event["event_type"] = "checkpoint_cleared";
		r_call.journal_event["barrier"] = barrier;
	}
	data["mutation"] = mutation;
	result["data"] = data;
	return result;
}

const Dictionary *SolersToolRegistry::_find_reversal(const String &p_reversal_id) const {
	for (const KeyValue<String, Vector<Dictionary>> &session : reversals_by_session) {
		for (const Dictionary &record : session.value) {
			if (String(record.get("id", String())) == p_reversal_id) {
				return &record;
			}
		}
	}
	return nullptr;
}

Dictionary SolersToolRegistry::_revert_latest(const SolersToolContext &p_context, const Dictionary &p_args) {
	const String reversal_id = String(p_args.get("reversal_id", String())).strip_edges();
	const String session_key = p_context.session_id.is_empty() ? String("direct") : p_context.session_id;
	Vector<Dictionary> *stack = reversals_by_session.getptr(session_key);
	if (!stack || stack->is_empty()) {
		return _error("REVERSAL_NOT_FOUND", "The reversal id is unknown or has already been used.");
	}
	const Dictionary record = stack->get(stack->size() - 1);
	if (String(record.get("id", String())) != reversal_id || String(record.get("session_id", String())) != session_key) {
		return _error("STALE_REVERSAL", "Only the latest Agent mutation at the current revision can be reverted.");
	}

	const uint64_t target_revision = MAX((int64_t)record.get("session_revision", 1) - 1, (int64_t)0);
	const Dictionary prepared = prepare_session_rewind(session_key, target_revision);
	if (!(bool)prepared.get("ok", false)) {
		return prepared;
	}
	Dictionary transaction = prepared.get("data", Dictionary());
	transaction["records"] = Array({ record });
	const Dictionary applied = apply_session_rewind(transaction);
	if (!(bool)applied.get("ok", false)) {
		abort_session_rewind(transaction);
		return applied;
	}
	finish_session_rewind(transaction);
	Dictionary data;
	data["reversal_id"] = reversal_id;
	data["reverted_session_revision"] = record.get("session_revision", 0);
	data["checkpoint_consumed"] = true;
	data["authored_state_changed"] = true;
	return _ok(data);
}

Dictionary SolersToolRegistry::_compact_addon_contract(const SolersToolContext &p_context, const Dictionary &p_result) {
	if (!(bool)p_result.get("ok", false)) {
		return p_result;
	}
	Dictionary result = p_result.duplicate(true);
	Dictionary data = result.get("data", Dictionary());
	const Dictionary contract = data.get("agent_contract", Dictionary());
	const String contract_id = contract.get("contract_id", String());
	if (contract_id.is_empty()) {
		return result;
	}
	const String delivery_key = p_context.session_id + ":" + contract_id;
	if (delivered_addon_contracts.has(delivery_key)) {
		Dictionary compact;
		compact["contract_id"] = contract_id;
		compact["unchanged"] = true;
		data["agent_contract"] = compact;
		result["data"] = data;
		return result;
	}
	delivered_addon_contracts.insert(delivery_key);
	return result;
}

Dictionary SolersToolRegistry::_inspect_engine(const Dictionary &p_args) {
	if (!reflection_service) {
		return _error("ENGINE_INSPECTION_UNAVAILABLE", "ClassDB inspection is unavailable.", false);
	}
	const Array classes = p_args.get("classes", Array());
	if (classes.is_empty()) {
		Dictionary search_args = p_args.duplicate(true);
		search_args.erase("classes");
		Dictionary result = reflection_service->search_classes(search_args);
		return result;
	}
	Array inspected_classes;
	Array errors;
	for (int i = 0; i < classes.size(); i++) {
		const Variant value = classes[i];
		const Dictionary inspected = reflection_service->introspect_class(Dictionary(value));
		if (!(bool)inspected.get("ok", false)) {
			Dictionary error;
			error["request_index"] = i;
			error["class_name"] = Dictionary(value).get("class_name", String());
			error["error"] = inspected.get("error", Dictionary());
			errors.push_back(error);
			continue;
		}
		Dictionary class_data = inspected.get("data", Dictionary());
		class_data["request_index"] = i;
		inspected_classes.push_back(class_data);
	}
	Dictionary data;
	data["classes"] = inspected_classes;
	data["errors"] = errors;
	data["requested_count"] = classes.size();
	data["complete"] = errors.is_empty();
	return _ok(data);
}

SolersTool *SolersToolRegistry::_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode, const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool || (*tool)->exposure() != SolersToolExposure::OPERATION) {
		return nullptr;
	}
	const SolersToolCapability &capability = (*tool)->capability();
	return capability.operation_domain == p_domain && capability.operation_mode == p_mode ? *tool : nullptr;
}

Array SolersToolRegistry::_operation_names(SolersOperationDomain p_domain, SolersOperationMode p_mode, const StringName &p_class, const StringName &p_kind) const {
	Vector<String> names;
	for (const KeyValue<StringName, SolersTool *> &entry : tools) {
		const SolersToolCapability &capability = entry.value->capability();
		if (entry.value->exposure() != SolersToolExposure::OPERATION || capability.operation_domain != p_domain || capability.operation_mode != p_mode ||
				(!p_class.is_empty() && (capability.target_class.is_empty() || !ClassDB::class_exists(p_class) || !ClassDB::is_parent_class(p_class, capability.target_class))) ||
				(!p_kind.is_empty() && capability.target_kind != p_kind)) {
			continue;
		}
		names.push_back(String(entry.key));
	}
	names.sort();
	Array out;
	for (const String &name : names) {
		out.push_back(name);
	}
	return out;
}

Array SolersToolRegistry::_operation_definitions(SolersOperationDomain p_domain, SolersOperationMode p_mode, const StringName &p_class, const StringName &p_kind) const {
	Array definitions;
	for (const Variant &name : _operation_names(p_domain, p_mode, p_class, p_kind)) {
		definitions.push_back(_tool_to_dictionary(tools[StringName(name)]));
	}
	return definitions;
}

Array SolersToolRegistry::_operation_summaries(SolersOperationDomain p_domain, SolersOperationMode p_mode) const {
	Array summaries;
	for (const Variant &name : _operation_names(p_domain, p_mode)) {
		const SolersTool *tool = tools[StringName(name)];
		const SolersToolCapability &capability = tool->capability();
		Dictionary summary;
		summary["name"] = name;
		summary["description"] = tool->description();
		if (p_mode == SolersOperationMode::QUERY) {
			summary["input_schema"] = tool->parameters_schema().duplicate(true);
		}
		if (!capability.target_class.is_empty()) {
			summary["target_class"] = capability.target_class;
		}
		if (!capability.target_kind.is_empty()) {
			summary["target_kind"] = capability.target_kind;
		}
		summaries.push_back(summary);
	}
	return summaries;
}

SolersToolMutationDomain SolersToolRegistry::_operation_domains(SolersOperationDomain p_domain, SolersOperationMode p_mode, const Dictionary &p_args) const {
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (!tool) {
		return SolersToolMutationDomain::NONE;
	}
	const SolersToolCapability &capability = tool->capability();
	const Dictionary arguments = p_args.get("arguments", Dictionary());
	return capability.mutation_domain_resolver ? capability.mutation_domain_resolver(arguments) : capability.mutation_domains;
}

Array SolersToolRegistry::_operation_resource_access(SolersOperationDomain p_domain, SolersOperationMode p_mode, const Dictionary &p_args) const {
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (tool && tool->capability().resource_access) {
		return tool->capability().resource_access(p_args.get("arguments", Dictionary()));
	}
	return Array({ Dictionary({ { "mode", p_mode == SolersOperationMode::QUERY ? "read" : "write" }, { "key", "*" } }) });
}

SolersToolExecution SolersToolRegistry::_operation_execution(SolersOperationDomain p_domain, SolersOperationMode p_mode, const Dictionary &p_args) const {
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (!tool) {
		return SolersToolExecution::MAIN_THREAD;
	}
	const SolersToolCapability &capability = tool->capability();
	const Dictionary arguments = p_args.get("arguments", Dictionary());
	return capability.execution_resolver ? capability.execution_resolver(arguments) : capability.execution;
}

Dictionary SolersToolRegistry::_validate_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode, const Dictionary &p_args) const {
	const StringName operation_name = StringName(p_args.get("operation", String()));
	if (p_mode == SolersOperationMode::QUERY && operation_name == SNAME("catalog")) {
		String error;
		if (!_validate_tool_schema_value(p_args.get("arguments", Dictionary()), _schema(R"({"type":"object","properties":{"operation":{"type":"string","minLength":1}},"additionalProperties":false})"), "arguments", error)) {
			return _error("TOOL_ARGUMENT_INVALID", error);
		}
		return Dictionary();
	}
	SolersTool *tool = _operation(p_domain, p_mode, operation_name);
	if (!tool) {
		return _error("OPERATION_UNAVAILABLE", vformat("Operation is not available through this authority: %s", operation_name));
	}
	auto with_contract = [this, tool](const Dictionary &p_error) {
		Dictionary invalid = p_error.duplicate(true);
		Dictionary data = invalid.get("data", Dictionary());
		data["operation"] = _tool_to_dictionary(tool);
		invalid["data"] = data;
		return invalid;
	};
	const Dictionary arguments = p_args.get("arguments", Dictionary());
	String error;
	if (!_validate_tool_schema_value(arguments, tool->parameters_schema(), "arguments", error)) {
		return with_contract(_error("TOOL_ARGUMENT_INVALID", error));
	}
	const SolersToolCapability &capability = tool->capability();
	if (capability.argument_validator) {
		const Dictionary invalid = capability.argument_validator(arguments);
		if (!invalid.is_empty()) {
			return with_contract(invalid);
		}
	}

	if (p_mode == SolersOperationMode::QUERY) {
		return Dictionary();
	}
	const Dictionary expected = p_args.get("expected_state", Dictionary());
	const SolersToolMutationDomain domains = _operation_domains(p_domain, p_mode, p_args);
	if (solers_has_mutation_domain(domains, SolersToolMutationDomain::EDITOR)) {
		const Dictionary actual = _solers_scene_state_receipt();
		if (!_solers_scene_state_matches(expected, actual)) {
			Dictionary failure = _error("SCENE_STATE_CONFLICT", "The edited scene changed since it was inspected.");
			failure["data"] = Dictionary({ { "expected_state", expected }, { "actual_state", actual } });
			return failure;
		}
	}
	if (solers_has_mutation_domain(domains, SolersToolMutationDomain::FILES)) {
		if (!expected.has("resources")) {
			return _error("RESOURCE_STATE_REQUIRED", "File mutations require expected_state.resources from object.query.");
		}
		Dictionary expected_sha;
		for (const Variant &item : Array(expected.get("resources", Array()))) {
			const Dictionary receipt = item;
			const String path = receipt.get("path", String());
			if (path.is_empty() || expected_sha.has(path)) {
				return _error("RESOURCE_STATE_INVALID", "expected_state.resources must contain unique resource paths.");
			}
			expected_sha[path] = receipt.get("sha256", String());
		}
		for (const Variant &item : _operation_resource_access(p_domain, p_mode, p_args)) {
			const Dictionary access = item;
			const String key = access.get("key", String());
			if (String(access.get("mode", "write")) != "write" || !key.begins_with("project:res://")) {
				continue;
			}
			const String path = key.trim_prefix("project:");
			const Dictionary state = _solers_checkpoint_target_state(file_checkpoint, path);
			if ((bool)state.get("existed", false) && String(expected_sha.get(path, String())) != String(state.get("content_sha256", String()))) {
				return _error("RESOURCE_STATE_CONFLICT", vformat("Resource changed since it was inspected: %s", path));
			}
		}
	}
	return Dictionary();
}

Dictionary SolersToolRegistry::_execute_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode, const SolersToolContext &p_context, const Dictionary &p_args) {
	if (p_mode == SolersOperationMode::QUERY && StringName(p_args.get("operation", String())) == SNAME("catalog")) {
		const StringName requested = StringName(Dictionary(p_args.get("arguments", Dictionary())).get("operation", String()));
		if (!requested.is_empty()) {
			SolersTool *tool = _operation(p_domain, SolersOperationMode::QUERY, requested);
			if (!tool) {
				tool = _operation(p_domain, SolersOperationMode::APPLY, requested);
			}
			return tool ? _ok(Dictionary({ { "operation", _tool_to_dictionary(tool) } })) : _error("OPERATION_UNAVAILABLE", vformat("Operation is not available through this authority: %s", requested));
		}
		return _ok(Dictionary({ { "queries", _operation_summaries(p_domain, SolersOperationMode::QUERY) }, { "operations", _operation_summaries(p_domain, SolersOperationMode::APPLY) } }));
	}
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (!tool) {
		return _error("OPERATION_UNAVAILABLE", "The validated operation is no longer registered.", false);
	}
	Dictionary result = tool->execute(p_context, p_args.get("arguments", Dictionary()));
	Dictionary data = result.get("data", Dictionary());
	if ((bool)result.get("ok", false) && String(data.get("status", String())) == "pending") {
		Dictionary poll_args = data.get("poll_args", Dictionary());
		poll_args["operation"] = String(tool->name());
		data["poll_args"] = poll_args;
		result["data"] = data;
	}
	return result;
}

Dictionary SolersToolRegistry::_poll_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode, const SolersToolContext &p_context, const Dictionary &p_args) {
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (!tool) {
		return _error("OPERATION_UNAVAILABLE", "The pending operation is no longer registered.", false);
	}
	Dictionary arguments = p_args.duplicate(true);
	arguments.erase("operation");
	return tool->poll(p_context, arguments);
}

bool SolersToolRegistry::_is_operation_ready(SolersOperationDomain p_domain, SolersOperationMode p_mode, const SolersToolContext &p_context, const Dictionary &p_args) const {
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (!tool) {
		return true;
	}
	Dictionary arguments = p_args.duplicate(true);
	arguments.erase("operation");
	return tool->is_continuation_ready(p_context, arguments);
}

void SolersToolRegistry::_complete_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode, const SolersToolContext &p_context, const Dictionary &p_args, const Dictionary &p_result) {
	SolersTool *tool = _operation(p_domain, p_mode, StringName(p_args.get("operation", String())));
	if (tool) {
		tool->complete(p_context, p_args.get("arguments", Dictionary()), p_result);
	}
}

static GameViewDebugger *_solers_game_view_debugger() {
	EditorData &editor_data = EditorNode::get_editor_data();
	for (int i = 0; i < editor_data.get_editor_plugin_count(); i++) {
		if (GameViewPluginBase *plugin = Object::cast_to<GameViewPluginBase>(editor_data.get_editor_plugin(i))) {
			return plugin->get_debugger().ptr();
		}
	}
	return nullptr;
}

Dictionary SolersToolRegistry::build_delivery_report(const Dictionary &p_args, int p_token_budget) const {
	ERR_FAIL_NULL_V(observation_service, Dictionary());
	Dictionary report = observation_service->inspect_project_delivery(p_args, p_token_budget);
	if (!resource_service) {
		return report;
	}
	const Dictionary export_result = resource_service->validate_export_presets(Dictionary({ { "debug", p_args.get("debug_export", false) } }));
	if (!(bool)export_result.get("ok", false)) {
		report["export"] = export_result;
		return report;
	}
	const Dictionary export_data = export_result.get("data", Dictionary());
	report["export"] = export_data;
	Array blockers = report.get("blockers", Array());
	Array advisories = report.get("advisories", Array());
	if ((int)export_data.get("preset_count", 0) == 0) {
		advisories.push_back(Dictionary({ { "code", "NO_EXPORT_PRESET" } }));
	} else if (!(bool)export_data.get("valid", false)) {
		blockers.push_back(Dictionary({ { "code", "INVALID_EXPORT_PRESET" }, { "error_count", export_data.get("error_count", 0) }, { "missing_template_count", export_data.get("missing_template_count", 0) } }));
	}
	report["blockers"] = blockers;
	report["advisories"] = advisories;
	const String session_id = p_args.get("_session_id", String());
	if (asset_service && !session_id.is_empty()) {
		HashSet<String> unreferenced;
		for (const Variant &item : Array(report.get("unreferenced_from_roots", Array()))) {
			unreferenced.insert(String(Dictionary(item).get("path", String())));
		}
		Array artifacts;
		for (const Variant &item : asset_service->list_assets()) {
			const Dictionary asset = item;
			if (String(asset.get("session_id", String())) != session_id || !(bool)asset.get("in_current_project", false)) {
				continue;
			}
			const Array files = Array(asset.get("project_entrypoints", Array())).is_empty() ? Array(asset.get("project_files", Array())) : Array(asset.get("project_entrypoints", Array()));
			bool consumed = false;
			for (const Variant &file : files) {
				consumed = consumed || !unreferenced.has(String(file));
			}
			if (!consumed) {
				artifacts.push_back(Dictionary({ { "asset_id", asset.get("id", String()) }, { "files", files }, { "sidecar", asset.get("sidecar_file", String()) } }));
			}
		}
		report["unconsumed_agent_artifacts"] = artifacts;
		if (!artifacts.is_empty()) {
			blockers.push_back(Dictionary({ { "code", "UNCONSUMED_AGENT_ARTIFACTS" }, { "count", artifacts.size() }, { "artifacts", artifacts } }));
			report["blockers"] = blockers;
		}
	}
	return report;
}

Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args, const String &p_call_id) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	ERR_FAIL_NULL_V(run_bar, _error("EDITOR_RUN_BAR_UNAVAILABLE", "The editor runtime controller is not available.", false));
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const String action = p_args.get("action", String());
	const bool was_playing = run_bar->is_playing();
	bool command_accepted = false;
	if (action == "set_input_actions") {
		ERR_FAIL_NULL_V(observation_service, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "Start the project before setting runtime input.");
		}
		const int64_t epoch = p_args.get("runtime_epoch", 0);
		const int64_t current_epoch = observation_service->get_runtime_status().get("runtime_epoch", 0);
		const int physics_frames = p_args.get("physics_frames", 0);
		const Variant observations = p_args.get("observations", Variant());
		if (p_call_id.is_empty() || epoch <= 0 || epoch != current_epoch || !p_args.has("actions") || physics_frames <= 0 || observations.get_type() != Variant::ARRAY || Array(observations).is_empty()) {
			return _error("STALE_RUNTIME_EPOCH", "set_input_actions requires actions, positive physics_frames, observations, and the current runtime_epoch returned by runtime.observe.");
		}
		observation_service->clear_runtime_control_result();
		debugger->send_message("solers:set_input_actions", { p_call_id, epoch, p_args["actions"], physics_frames, observations });
		Dictionary poll_args;
		poll_args["action"] = action;
		poll_args["call_id"] = p_call_id;
		poll_args["runtime_epoch"] = epoch;
		Dictionary pending;
		pending["status"] = "pending";
		pending["poll_args"] = poll_args;
		return _ok(pending);
	}
	if (action == "set_property") {
		ERR_FAIL_NULL_V(observation_service, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "Start the project before editing runtime state.");
		}
		if (observation_service->has_runtime_query()) {
			return _error("RUNTIME_QUERY_BUSY", "Wait for the active native runtime observation before changing runtime state.");
		}
		const uint64_t epoch = (int64_t)p_args.get("runtime_epoch", 0);
		const NodePath node_path = NodePath(p_args.get("node_path", String()));
		ObjectID object_id;
		const bool valid_object_id = solers_object_id_from_variant(p_args.get("object_id", Variant()), object_id);
		const StringName property = p_args.get("property", String());
		const String observation_id = p_args.get("observation_id", String());
		if (node_path.is_empty() || !node_path.is_absolute() || !valid_object_id || property.is_empty() || observation_id.is_empty() || !p_args.has("value")) {
			return _error("INVALID_ARGUMENT", "set_property requires runtime_epoch, absolute node_path, object_id, property, observation_id, and value from runtime.observe.");
		}
		Variant before;
		PropertyInfo property_info;
		String cached_observation_id;
		if (!observation_service->get_runtime_property(epoch, node_path, object_id, property, before, property_info, cached_observation_id) || cached_observation_id != observation_id) {
			return _error("STALE_RUNTIME_OBSERVATION", "Observe this exact runtime node property in the current epoch before changing it.");
		}
		Variant value;
		String coercion_error;
		if (!solers_coerce_variant_value(property_info, p_args.get("value", Variant()), value, coercion_error)) {
			return _error("INVALID_PROPERTY_VALUE", coercion_error);
		}
		Dictionary query_args;
		query_args["target"] = "scene";
		Array node_paths;
		node_paths.push_back(String(node_path));
		query_args["node_paths"] = node_paths;
		Array properties;
		properties.push_back(property);
		query_args["properties"] = properties;
		Dictionary pending = observation_service->observe_runtime(query_args);
		if (pending.get("status", String()) != "pending") {
			return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not start post-write verification.");
		}
		Dictionary poll_args = pending.get("poll_args", Dictionary());
		poll_args["action"] = action;
		poll_args["runtime_epoch"] = (int64_t)epoch;
		poll_args["node_path"] = String(node_path);
		poll_args["object_id"] = solers_object_id_to_string(object_id);
		poll_args["property"] = property;
		poll_args["before"] = before;
		poll_args["value"] = value;
		poll_args["phase"] = "prewrite";
		pending["poll_args"] = poll_args;
		return _ok(pending);
	}
	if (action == "play_current_scene") {
		if (!was_playing && !editor_interface->get_edited_scene_root()) {
			return _error("CURRENT_SCENE_UNAVAILABLE", "Open a scene before starting the project.");
		}
		if (!was_playing) {
			run_bar->play_current_scene();
		}
		command_accepted = true;
	} else if (action == "stop") {
		if (was_playing) {
			run_bar->stop_playing();
		}
		command_accepted = true;
	} else if (action == "suspend" || action == "resume" || action == "next_frame") {
		GameViewDebugger *game_debugger = _solers_game_view_debugger();
		if (!debugger || !debugger->is_session_active() || !game_debugger) {
			return _error("RUNTIME_NOT_CONNECTED", "The native Game View debugger is not connected.");
		}
		if (action == "next_frame") {
			game_debugger->next_frame();
		} else {
			game_debugger->set_suspend(action == "suspend");
		}
		command_accepted = true;
	} else if (action == "debug_break") {
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "The native script debugger is not connected.");
		}
		if (!debugger->is_breaked()) {
			debugger->debug_break();
		}
		command_accepted = true;
	} else if (action == "debug_continue" || action == "debug_step" || action == "debug_next" || action == "debug_out") {
		if (!debugger || !debugger->is_debuggable()) {
			return _error("RUNTIME_NOT_BREAKED", "Break at a debuggable stack frame before stepping or continuing.");
		}
		if (action == "debug_continue") {
			debugger->debug_continue();
		} else if (action == "debug_step") {
			debugger->debug_step();
		} else if (action == "debug_next") {
			debugger->debug_next();
		} else {
			debugger->debug_out();
		}
		command_accepted = true;
	} else {
		return _error("INVALID_ARGUMENT", "Unknown runtime action.");
	}
	if (command_accepted) {
		Dictionary data;
		data["action"] = action;
		data["command_accepted"] = true;
		data["runtime_epoch"] = observation_service ? (int64_t)observation_service->get_runtime_status().get("runtime_epoch", 0) : 0;
		return _ok(data);
	}

	return _error("RUNTIME_CONTROL_FAILED", "The native runtime command was not accepted.", false);
}

bool SolersToolRegistry::_is_runtime_control_ready(const Dictionary &p_args) const {
	const String action = p_args.get("action", String());
	if (action == "set_input_actions") {
		if (!observation_service) {
			return true;
		}
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		return !debugger || !debugger->is_session_active() ||
				(int64_t)observation_service->get_runtime_status().get("runtime_epoch", 0) != (int64_t)p_args.get("runtime_epoch", 0) ||
				!observation_service->get_runtime_control_result(p_args.get("call_id", String())).is_empty();
	}
	if (action == "set_property") {
		return !observation_service || observation_service->is_runtime_observation_ready(p_args);
	}
	return true;
}

Dictionary SolersToolRegistry::_poll_runtime_control(const Dictionary &p_args) const {
	const String action = p_args.get("action", String());
	if (action == "set_input_actions") {
		ERR_FAIL_NULL_V(observation_service, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
		ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
		const int64_t epoch = p_args.get("runtime_epoch", 0);
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "The runtime stopped before applying input.");
		}
		if ((int64_t)observation_service->get_runtime_status().get("runtime_epoch", 0) != epoch) {
			return _error("STALE_RUNTIME_EPOCH", "The runtime epoch changed before applying input.");
		}
		const Dictionary result = observation_service->get_runtime_control_result(p_args.get("call_id", String()));
		if (result.is_empty()) {
			Dictionary pending;
			pending["status"] = "pending";
			pending["poll_args"] = p_args;
			return _ok(pending);
		}
		if (!(bool)result.get("ok", false)) {
			return _error(result.get("code", "RUNTIME_INPUT_REJECTED"), result.get("message", "The runtime rejected the input state."));
		}
		Dictionary data;
		data["action"] = action;
		data["runtime_epoch"] = epoch;
		data["input_state_applied"] = true;
		data["physics_frames"] = result.get("physics_frames", 0);
		data["before"] = result.get("before", Array());
		data["after"] = result.get("after", Array());
		data["availability"] = result.get("availability", Dictionary());
		return _ok(data);
	}
	if (action == "set_property") {
		ERR_FAIL_NULL_V(observation_service, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		Dictionary observed = observation_service->observe_runtime(p_args);
		if (observed.get("status", String()) == "pending") {
			return _ok(observed);
		}
		if (String(Dictionary(observed.get("availability", Dictionary())).get("state", String())) != "complete") {
			return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not read the runtime property.");
		}
		const NodePath node_path = NodePath(p_args.get("node_path", String()));
		ObjectID object_id;
		if (!solers_object_id_from_variant(p_args.get("object_id", Variant()), object_id)) {
			return _error("RUNTIME_CONTINUATION_INVALID", "The runtime object_id is invalid.", false);
		}
		const StringName property = p_args.get("property", String());
		Variant current;
		PropertyInfo property_info;
		String observation_id;
		if (!observation_service->get_runtime_property((int64_t)p_args.get("runtime_epoch", 0), node_path, object_id, property, current, property_info, observation_id)) {
			return _error("RUNTIME_OBJECT_DISAPPEARED", "The canonical runtime node or property disappeared before verification.");
		}
		if (String(p_args.get("phase", String())) == "prewrite") {
			if (current != p_args.get("before", Variant())) {
				return _error("RUNTIME_STATE_CONFLICT", "The runtime property changed after it was observed; observe it again before writing.");
			}
			EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
			ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
			if (!debugger || !debugger->is_session_active() || (int64_t)observation_service->get_runtime_status().get("runtime_epoch", 0) != (int64_t)p_args.get("runtime_epoch", 0)) {
				return _error("STALE_RUNTIME_EPOCH", "The runtime changed before the property write.");
			}
			debugger->update_remote_object(object_id, property, p_args.get("value", Variant()));
			Dictionary query_args;
			query_args["target"] = "scene";
			Array node_paths;
			node_paths.push_back(String(node_path));
			query_args["node_paths"] = node_paths;
			Array requested_properties;
			requested_properties.push_back(property);
			query_args["properties"] = requested_properties;
			Dictionary pending = observation_service->observe_runtime(query_args);
			if (pending.get("status", String()) != "pending") {
				return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not start post-write verification.");
			}
			Dictionary poll_args = pending.get("poll_args", Dictionary());
			for (const char *key : { "action", "runtime_epoch", "node_path", "object_id", "property", "before", "value" }) {
				poll_args[key] = p_args.get(key, Variant());
			}
			poll_args["phase"] = "postwrite";
			pending["poll_args"] = poll_args;
			return _ok(pending);
		}
		if (String(p_args.get("phase", String())) != "postwrite") {
			return _error("RUNTIME_CONTINUATION_INVALID", "Unknown runtime property transaction phase.", false);
		}
		if (current != p_args.get("value", Variant())) {
			return _error("RUNTIME_POSTCONDITION_FAILED", "Godot did not retain the requested runtime property value.");
		}
		Dictionary data;
		data["action"] = action;
		data["runtime_only"] = true;
		data["runtime_epoch"] = observed.get("runtime_epoch", 0);
		data["node_path"] = String(node_path);
		data["object_id"] = solers_object_id_to_string(object_id);
		data["property"] = property;
		data["before"] = solers_summarize_display_value(p_args.get("before", Variant()));
		data["after"] = solers_summarize_display_value(current);
		return _ok(data);
	}
	return _error("RUNTIME_CONTINUATION_INVALID", "Only runtime property verification has a continuation.", false);
}

void SolersToolRegistry::_register_observation_tools() {
	if (!observation_service) {
		return;
	}
	SolersObservationService *obs = observation_service;

	_add_observe_exposed("project.search", "Discover project paths or search text files on the worker. For the state and mutation receipt of a known res:// path, use object.query target=path instead. An empty path query lists project paths; text and symbol require a query. Results use a stable cursor.", R"({"type":"object","properties":{"type":{"type":"string","enum":["path","text","symbol"]},"query":{"type":"string","description":"Case-insensitive path/text query; may be empty only for type=path."},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1,"description":"Optional page size; the result budget remains authoritative."}},"required":["type","query"],"additionalProperties":false})", SolersToolExposure::OPERATION, [this, obs](const SolersToolContext &ctx, const Dictionary &a) {
				const String type = a.get("type", String());
				if (type != "path" && String(a.get("query", String())).is_empty()) {
					return _error("INVALID_ARGUMENT", "query is required for text and symbol search.");
				}
				return _ok(obs->search_project(a, ctx.result_token_budget)); }, {}, {}, {}, SolersToolUiKind::SEARCH, SolersToolExecution::WORKER_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	_add_observe_exposed("project.read_file", "Read a bounded line range from a project text file. Continue from next_line; PackedScene defaults to a native digest unless raw=true is required for source editing.", R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to read."},"line_start":{"type":"integer","minimum":1},"line_count":{"type":"integer","minimum":1},"raw":{"type":"boolean","description":"Only for PackedScene source editing. Default false."}},"required":["path"],"additionalProperties":false})", SolersToolExposure::OPERATION, [this, obs](const SolersToolContext &ctx, const Dictionary &a) {
				const Dictionary file = obs->read_project_file(a.get("path", String()), (int)a.get("line_start", 1), (int)a.get("line_count", 200), (bool)a.get("raw", false), ctx.result_token_budget);
				if (!(bool)file.get("ok", false)) {
					Dictionary error;
					error["code"] = file.get("code", "READ_FAILED");
					error["message"] = file.get("error", String("Unable to read file."));
					error["recoverable"] = true;
					Dictionary result;
					result["ok"] = false;
					result["error"] = error;
					if (file.has("digest")) {
						Dictionary data;
						data["digest"] = file["digest"];
						data["path"] = file.get("path", String());
						result["data"] = data;
					}
					return result;
				}
				return _ok(file); }, _access_by_arg("read", "project:", "path"), {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	_add_observe_exposed("runtime.observe", "Observe one canonical runtime snapshot through Godot's native debugger. Scene returns typed property receipts; spatial returns post-draw subtree AABBs, camera projection, and physics ray facts.", R"({"type":"object","properties":{"target":{"type":"string","enum":["scene","spatial","stack","performance"]},"node_paths":{"type":"array","items":{"type":"string","pattern":"^/"},"maxItems":64,"uniqueItems":true},"focus_paths":{"type":"array","items":{"type":"string","pattern":"^/"},"minItems":1,"maxItems":32,"uniqueItems":true},"path_prefix":{"type":"string","pattern":"^/"},"name_contains":{"type":"string"},"class_name":{"type":"string"},"cursor":{"type":"integer","minimum":0},"properties":{"type":"array","items":{"type":"string","minLength":1},"maxItems":64,"uniqueItems":true},"max_results":{"type":"integer","minimum":1}},"required":["target"],"additionalProperties":false})", SolersToolExposure::OPERATION, [this, obs](const SolersToolContext &ctx, const Dictionary &a) { return _ok(obs->observe_runtime(a, ctx.result_token_budget)); }, {}, [this, obs](const SolersToolContext &ctx, const Dictionary &a) { return _ok(obs->observe_runtime(a, ctx.result_token_budget)); }, [obs](const SolersToolContext &, const Dictionary &a) { return obs->is_runtime_observation_ready(a); }, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::RUNTIME, SolersOperationMode::QUERY);
	_add_observe_exposed("project.delivery_report", "Inspect current project delivery facts through ProjectSettings, ResourceLoader, EditorFileSystem, InputMap, UndoRedo, and EditorExport. Unreferenced and duplicate files are advisories, never automatic deletion decisions.", R"({"type":"object","properties":{"roots":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","pattern":"^res://"},"description":"Additional authoritative roots for dynamically loaded content."},"debug_export":{"type":"boolean"}},"additionalProperties":false})", SolersToolExposure::OPERATION, [this](const SolersToolContext &ctx, const Dictionary &a) { Dictionary args = a; args["_session_id"] = ctx.session_id; return _ok(build_delivery_report(args, ctx.result_token_budget)); }, {}, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	SolersToolHostPolicy capture_host;
	capture_host.required_model_inputs.push_back("image");
	_add_observe_exposed("render.capture", "Capture content-addressed visual evidence from an explicit native state. Edited-scene receipts bind pixels to the World3D fingerprint; runtime receipts bind pixels to the runtime epoch. Spatial facts are observed independently. debug_draw uses Godot's Viewport enum; inspect it with engine.describe.", R"({"type":"object","properties":{"target":{"type":"string","enum":["editor","camera","top_down","orthographic","runtime"]},"source_state":{"type":"object","properties":{"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0},"root_object_id":{"type":"string","pattern":"^-?[0-9]+$"}},"required":["history_id","version"],"additionalProperties":true},"node_path":{"type":"string"},"axis":{"type":"string","enum":["x","y","z"]},"direction":{"type":"string","enum":["positive","negative"]},"focus_paths":{"type":"array","items":{"type":"string"}},"section_position":{"type":"number"},"debug_draw":{"type":"integer","minimum":0}},"required":["target"],"additionalProperties":false})", SolersToolExposure::MODEL, [obs](const SolersToolContext &, const Dictionary &a) { return obs->capture_viewport(a); }, {}, [obs](const SolersToolContext &, const Dictionary &a) { return obs->poll_viewport_capture(a); }, [obs](const SolersToolContext &, const Dictionary &a) { return obs->is_viewport_capture_ready(a); }, SolersToolUiKind::CAPTURE, SolersToolExecution::MAIN_THREAD, capture_host);

	if (resource_service) {
		SolersResourceService *svc = resource_service;
		_add_observe_exposed("export.list_presets", "List Godot export platforms and export presets from the current project.", R"({"type":"object","properties":{"include_platforms":{"type":"boolean","description":"Include available export platforms. Default true."}}})", SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) { return svc->list_export_presets(a); }, {}, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
		_add_observe_exposed("export.validate_presets", "Validate configured export presets without exporting build artifacts.", R"({"type":"object","properties":{"debug":{"type":"boolean","description":"Validate against the debug export template. Default false."}}})", SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_export_presets(a); }, {}, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
		_add_operation(SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY, "export.run_preset", "Run Godot's native EditorExportPlatform::export_project for one export preset.",
				R"({"type":"object","properties":{"preset_index":{"type":"integer","description":"Export preset index from export.list_presets."},"preset_name":{"type":"string","description":"Export preset name when index is unknown."},"debug":{"type":"boolean","description":"Export debug build. Default false."},"export_path":{"type":"string","description":"Optional output path override; defaults to the preset export_path."}}})",
				SolersPermissionManager::PERMISSION_EXPORT_BUILD, SolersToolMutationDomain::IRREVERSIBLE,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_export_preset(a); }, {});
	}
}

void SolersToolRegistry::_register_script_tools() {
	if (!script_service) {
		return;
	}
	SolersScriptService *svc = script_service;
	Vector<String> project_redact;
	project_redact.push_back("content");
	_add("project.settings", "Edit ProjectSettings values through the live engine singleton.", R"({"type":"object","properties":{"values":{"type":"object"},"erase":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::EDITOR, Vector<String>(), SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) {
			Dictionary args = a.duplicate(true);
			args["operation"] = "settings";
			return svc->edit_project(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) { return Array({ Dictionary({ { "mode", "write" }, { "key", "project:res://project.godot" } }) }); }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY);
	_add("project.path", "Write an ordinary project data file, create a directory, or remove a file or directory through Godot's native editor lifecycle. Inspect the exact path with object.query target=path and pass its returned expected_state to editor.apply.", R"({"type":"object","properties":{"action":{"type":"string","enum":["write","create_directory","remove"]},"path":{"type":"string","pattern":"^res://"},"content":{"type":"string"}},"required":["action","path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, project_redact, SolersToolExposure::OPERATION, [this, svc](const SolersToolContext &, const Dictionary &a) {
			const String action = a.get("action", String());
			if (action == "remove") {
				return file_checkpoint ? file_checkpoint->remove_project_path(a.get("path", String())) : _error("FILE_CHECKPOINT_UNAVAILABLE", "File checkpoint service is unavailable.", false);
			}
			Dictionary args = a.duplicate(true);
			args["operation"] = action == "write" ? "write_file" : action;
			args.erase("action");
			return svc->edit_project(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "project:" + String(a.get("path", String()));
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, {}, SNAME("path"), [](const Dictionary &a) {
				if (String(a.get("action", String())) != "remove") {
					return true;
				}
				EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
				return !filesystem || !filesystem->is_scanning(); });

	Vector<String> script_redact;
	script_redact.push_back("content");
	script_redact.push_back("old_text");
	script_redact.push_back("new_text");
	_add("script.edit", "Create a script or replace one exact text block. Replace requires expected_sha256 from the latest project.read_file plus unique byte-for-byte old_text. A stale hash fails without changing the file; successful writes are parser-validated, checkpointed, and return the new persisted hash.", R"({"type":"object","properties":{"operation":{"type":"string","enum":["create","replace"]},"path":{"type":"string","pattern":"^res://.*\\.(gd|cs|gdshader|gdshaderinc)$"},"content":{"type":"string"},"old_text":{"type":"string","minLength":1},"new_text":{"type":"string"},"expected_sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"}},"required":["operation","path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, script_redact, SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_script(a); }, SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"), {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY);
	_add_observe_exposed("script.validate", "Validate script source through Godot's registered ScriptLanguage implementation.", R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the script to validate."},"source":{"type":"string","description":"Optional source override; validates this text instead of the file content."}},"required":["path"]})", SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_script(a); }, {}, {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
}

void SolersToolRegistry::_register_runtime_tools() {
	const SolersPermissionManager::Permission run_project = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	_add_operation(SolersOperationDomain::RUNTIME, SolersOperationMode::APPLY, "runtime.control", "Control Godot's active debugger, apply the complete Solers-owned input action state across declared physics frames with before/after property facts, or make one preconditioned runtime-only property change. Omitted input actions are released; an empty actions array releases all.", R"({"type":"object","properties":{"action":{"type":"string","enum":["play_current_scene","stop","suspend","resume","next_frame","debug_break","debug_continue","debug_step","debug_next","debug_out","set_input_actions","set_property"]},"runtime_epoch":{"type":"integer","minimum":0},"actions":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1},"strength":{"type":"number","exclusiveMinimum":0,"maximum":1}},"required":["name","strength"],"additionalProperties":false}},"physics_frames":{"type":"integer","minimum":1},"observations":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"node_path":{"type":"string","pattern":"^/"},"properties":{"type":"array","minItems":1,"maxItems":32,"uniqueItems":true,"items":{"type":"string","minLength":1}}},"required":["node_path","properties"],"additionalProperties":false}},"node_path":{"type":"string","pattern":"^/"},"object_id":{"type":"string","pattern":"^-?[0-9]+$"},"property":{"type":"string","minLength":1},"observation_id":{"type":"string","minLength":64,"maxLength":64},"value":{}},"required":["action"],"additionalProperties":false})", run_project, SolersToolMutationDomain::IRREVERSIBLE, [this](const SolersToolContext &ctx, const Dictionary &a) { return _run_control(a, ctx.call_id); }, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "runtime:";
				accesses.push_back(access);
				return accesses; }, StringName(), [this](const SolersToolContext &, const Dictionary &a) { return _poll_runtime_control(a); }, [this](const SolersToolContext &, const Dictionary &a) { return _is_runtime_control_ready(a); });
}

static Dictionary _solers_apply_plugin_mention(const SolersToolContext &p_context, const Dictionary &p_args, const String &p_capability) {
	if (!String(p_args.get("provider", String())).strip_edges().is_empty()) {
		return p_args;
	}
	const String kind = String(p_args.get("kind", String())).strip_edges().to_lower();
	for (int i = 0; i < p_context.mentions.size(); i++) {
		const Dictionary mention = p_context.mentions[i];
		const String source = String(mention.get("source", "plugin")).strip_edges().to_lower();
		if (!source.is_empty() && source != "plugin") {
			continue;
		}
		SolersPlugin *plugin = SolersPluginRegistry::get_plugin(mention.get("id", String()));
		const Dictionary profile = plugin ? plugin->get_profile() : Dictionary();
		if (!plugin || !(bool)profile.get(p_capability, false) || (!kind.is_empty() && !Array(profile.get("kinds", Array())).has(kind))) {
			continue;
		}
		Dictionary args = p_args.duplicate(true);
		args["provider"] = profile.get("id", String());
		return args;
	}
	return p_args;
}

void SolersToolRegistry::_register_asset_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *svc = asset_service;
	Array generation_plugin_ids;
	Array operation_plugin_ids;
	Array generation_kinds;
	Array catalog_plugin_ids;
	Array catalog_kinds;
	Dictionary option_schemas_by_name;
	String generation_labels;
	String catalog_labels;
	auto append_unique = [](Array &r_values, const Variant &p_value) {
		if (!r_values.has(p_value)) {
			r_values.push_back(p_value);
		}
	};

	for (SolersPlugin *plugin : SolersPluginRegistry::get_plugins()) {
		const Dictionary profile = plugin->get_profile();
		const String id = String(profile.get("id", String())).strip_edges().to_lower();
		const String label = String(profile.get("label", id));
		const Array kinds = profile.get("kinds", Array());
		if (!plugin->get_operation_defs().is_empty()) {
			append_unique(operation_plugin_ids, id);
		}
		if ((bool)profile.get("supports_generation", false)) {
			append_unique(generation_plugin_ids, id);
			generation_labels += generation_labels.is_empty() ? label : ", " + label;
			for (int i = 0; i < kinds.size(); i++) {
				const String kind = String(kinds[i]).to_lower();
				append_unique(generation_kinds, kind);
				const Dictionary schema = plugin->get_generation_options_schema(kind);
				for (const Variant *key = schema.next(nullptr); key; key = schema.next(key)) {
					const String name = String(*key);
					Dictionary option = Dictionary(schema[*key]).duplicate(true);
					const String description = String(option.get("description", String()));
					option["description"] = description.is_empty() ? label : label + ": " + description;
					Array variants = option_schemas_by_name.get(name, Array());
					const String encoded = JSON::stringify(option);
					bool duplicate = false;
					for (int variant = 0; variant < variants.size(); variant++) {
						if (JSON::stringify(variants[variant]) == encoded) {
							duplicate = true;
							break;
						}
					}
					if (!duplicate) {
						variants.push_back(option);
						option_schemas_by_name[name] = variants;
					}
				}
			}
		}
		if ((bool)profile.get("supports_catalog", false)) {
			append_unique(catalog_plugin_ids, id);
			catalog_labels += catalog_labels.is_empty() ? label : ", " + label;
			for (int i = 0; i < kinds.size(); i++) {
				append_unique(catalog_kinds, String(kinds[i]).to_lower());
			}
		}
	}

	Dictionary provider_option_properties;
	for (const Variant *key = option_schemas_by_name.next(nullptr); key; key = option_schemas_by_name.next(key)) {
		const Array variants = option_schemas_by_name[*key];
		if (variants.size() == 1) {
			provider_option_properties[*key] = variants[0];
		} else {
			Dictionary union_schema;
			union_schema["anyOf"] = variants;
			provider_option_properties[*key] = union_schema;
		}
	}
	Dictionary provider_options_schema;
	provider_options_schema["type"] = "object";
	provider_options_schema["description"] = "Options defined by the selected Solers plugin.";
	provider_options_schema["properties"] = provider_option_properties;
	provider_options_schema["additionalProperties"] = true;

	Dictionary import_profile_schema;
	import_profile_schema["type"] = "string";
	Array import_profiles;
	import_profiles.push_back("runtime");
	import_profiles.push_back("baked_static");
	import_profile_schema["enum"] = import_profiles;
	import_profile_schema["description"] = "Godot import intent. baked_static enables native lightmap UV2 generation for 3D assets.";
	Dictionary target_dir_schema;
	target_dir_schema["type"] = "string";
	target_dir_schema["description"] = "Optional res:// destination. Defaults to res://assets/<kind>/<name>-<job suffix>.";
	Dictionary max_triangles_schema;
	max_triangles_schema["type"] = "integer";
	max_triangles_schema["minimum"] = 0;
	max_triangles_schema["description"] = "Optional 3D source triangle budget. Zero disables the budget only when the project ceiling permits it.";
	Dictionary map_types_schema;
	map_types_schema["type"] = "array";
	Dictionary map_type_item;
	map_type_item["type"] = "string";
	map_types_schema["items"] = map_type_item;
	map_types_schema["uniqueItems"] = true;
	map_types_schema["description"] = "Optional exact material map roles to import; omitted roles are not copied.";

	Dictionary catalog_search_schema;
	catalog_search_schema["type"] = "object";
	Dictionary catalog_search_properties;
	Dictionary catalog_provider_schema;
	catalog_provider_schema["type"] = "string";
	catalog_provider_schema["enum"] = catalog_plugin_ids;
	catalog_provider_schema["description"] = "Registered catalog plugin.";
	catalog_search_properties["provider"] = catalog_provider_schema;
	Dictionary catalog_kind_schema;
	catalog_kind_schema["type"] = "string";
	catalog_kind_schema["enum"] = catalog_kinds;
	catalog_search_properties["kind"] = catalog_kind_schema;
	Dictionary query_schema;
	query_schema["type"] = "string";
	catalog_search_properties["query"] = query_schema;
	Dictionary limit_schema;
	limit_schema["type"] = "integer";
	limit_schema["minimum"] = 1;
	limit_schema["maximum"] = 50;
	catalog_search_properties["limit"] = limit_schema;
	Dictionary offset_schema;
	offset_schema["type"] = "integer";
	offset_schema["minimum"] = 0;
	catalog_search_properties["offset"] = offset_schema;
	Dictionary refresh_schema;
	refresh_schema["type"] = "boolean";
	catalog_search_properties["refresh"] = refresh_schema;
	catalog_search_schema["properties"] = catalog_search_properties;
	Array catalog_search_required;
	catalog_search_required.push_back("query");
	catalog_search_required.push_back("kind");
	catalog_search_schema["required"] = catalog_search_required;
	catalog_search_schema["additionalProperties"] = false;
	const CharString catalog_search_json = JSON::stringify(catalog_search_schema).utf8();
	const CharString catalog_search_description = vformat("Browse or search lightweight metadata through a registered catalog plugin (%s). Inspect a selected result before acquiring it.", catalog_labels).utf8();
	_add("asset.catalog.search", catalog_search_description.get_data(), catalog_search_json.get_data(), SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::OPERATION, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_search(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-directory:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower();
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);

	Dictionary catalog_inspect_schema;
	catalog_inspect_schema["type"] = "object";
	Dictionary catalog_inspect_properties;
	catalog_inspect_properties["provider"] = catalog_provider_schema;
	catalog_inspect_properties["kind"] = catalog_kind_schema;
	Dictionary source_asset_schema;
	source_asset_schema["type"] = "string";
	source_asset_schema["minLength"] = 1;
	catalog_inspect_properties["asset_id"] = source_asset_schema;
	catalog_inspect_properties["refresh"] = refresh_schema;
	catalog_inspect_schema["properties"] = catalog_inspect_properties;
	Array catalog_inspect_required;
	catalog_inspect_required.push_back("kind");
	catalog_inspect_required.push_back("asset_id");
	catalog_inspect_schema["required"] = catalog_inspect_required;
	catalog_inspect_schema["additionalProperties"] = false;
	const CharString catalog_inspect_json = JSON::stringify(catalog_inspect_schema).utf8();
	_add("asset.catalog.inspect", "Resolve one exact catalog result into authoritative variants, dependencies, licensing, and checksums. asset.catalog.acquire accepts only a previously inspected variant.", catalog_inspect_json.get_data(), SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::OPERATION, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_inspect(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-detail:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower() + ":" + String(a.get("asset_id", String())).to_lower();
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);

	Dictionary generate_schema;
	generate_schema["type"] = "object";
	Dictionary generate_properties;
	Dictionary generation_kind_schema;
	generation_kind_schema["type"] = "string";
	generation_kind_schema["enum"] = generation_kinds;
	generate_properties["kind"] = generation_kind_schema;
	Dictionary prompt_schema;
	prompt_schema["type"] = "string";
	prompt_schema["description"] = "Generation prompt. With reference images prefer one short sentence (identity/style/pose only); long multi-constraint essays dilute Image-to-3D. Text-to-3D may be slightly richer but stay concise.";
	generate_properties["prompt"] = prompt_schema;
	Dictionary attachments_schema;
	attachments_schema["type"] = "array";
	Dictionary attachment_item;
	attachment_item["type"] = "string";
	attachments_schema["items"] = attachment_item;
	attachments_schema["uniqueItems"] = true;
	attachments_schema["description"] = "Attachment ids from the current conversation, when supported by the selected plugin.";
	generate_properties["input_attachments"] = attachments_schema;
	Dictionary name_schema;
	name_schema["type"] = "string";
	generate_properties["name"] = name_schema;
	Dictionary profile_schema;
	profile_schema["type"] = "string";
	generate_properties["profile"] = profile_schema;
	Dictionary generation_provider_schema;
	generation_provider_schema["type"] = "string";
	generation_provider_schema["enum"] = generation_plugin_ids;
	generation_provider_schema["description"] = "Optional registered generation plugin. Explicit selection overrides @mention and configured defaults.";
	generate_properties["provider"] = generation_provider_schema;
	generate_properties["provider_options"] = provider_options_schema;
	generate_properties["target_dir"] = target_dir_schema;
	generate_properties["import_profile"] = import_profile_schema;
	generate_properties["max_triangles"] = max_triangles_schema;
	generate_properties["map_types"] = map_types_schema;
	generate_schema["properties"] = generate_properties;
	Array generate_required;
	generate_required.push_back("kind");
	generate_schema["required"] = generate_required;
	generate_schema["additionalProperties"] = false;
	const CharString generate_json = JSON::stringify(generate_schema).utf8();
	const CharString generate_description = vformat("Generate an asset through a registered Solers plugin (%s), persist its output in the global Studio vault, and import it into res://. Read the selected plugin's provider_options schema; the job is terminal only after Godot verifies imported resources.", generation_labels).utf8();
	SolersToolHostPolicy generate_host;
	generate_host.attachment_args.push_back("input_attachments");
	_add("asset.generate", generate_description.get_data(), generate_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::OPERATION, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->generate_for_session(_solers_apply_plugin_mention(ctx, a, "supports_generation"), ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary job;
				job["mode"] = "write";
				job["key"] = "asset-job:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower() + ":" + String(a.get("name", String())).to_lower();
				accesses.push_back(job);
				Dictionary project;
				project["mode"] = "write";
				const String target = String(a.get("target_dir", String())).strip_edges();
				project["key"] = "project:" + (target.is_empty() ? "res://assets/" + String(a.get("kind", "asset")) : target.replace_char('\\', '/').simplify_path());
				accesses.push_back(project);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, generate_host, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);

	Dictionary acquire_schema;
	acquire_schema["type"] = "object";
	Dictionary acquire_properties;
	acquire_properties["provider"] = catalog_provider_schema;
	acquire_properties["kind"] = catalog_kind_schema;
	acquire_properties["asset_id"] = source_asset_schema;
	Dictionary variant_schema;
	variant_schema["type"] = "string";
	variant_schema["minLength"] = 1;
	acquire_properties["variant"] = variant_schema;
	Dictionary source_version_schema;
	source_version_schema["type"] = "string";
	acquire_properties["source_version"] = source_version_schema;
	acquire_properties["name"] = name_schema;
	acquire_properties["target_dir"] = target_dir_schema;
	acquire_properties["import_profile"] = import_profile_schema;
	acquire_properties["max_triangles"] = max_triangles_schema;
	acquire_properties["map_types"] = map_types_schema;
	acquire_schema["properties"] = acquire_properties;
	Array acquire_required;
	acquire_required.push_back("kind");
	acquire_required.push_back("asset_id");
	acquire_required.push_back("variant");
	acquire_schema["required"] = acquire_required;
	acquire_schema["additionalProperties"] = false;
	const CharString acquire_json = JSON::stringify(acquire_schema).utf8();
	_add("asset.catalog.acquire", "Acquire one exact inspected catalog variant, verify its source metadata and checksums, then import it directly into res:// and write project-local license/attribution metadata.", acquire_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::OPERATION, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_acquire(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary job;
				job["mode"] = "write";
				job["key"] = "asset-job:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("asset_id", String())) + ":" + String(a.get("variant", String()));
				accesses.push_back(job);
				Dictionary project;
				project["mode"] = "write";
				const String target = String(a.get("target_dir", String())).strip_edges();
				project["key"] = "project:" + (target.is_empty() ? "res://assets/" + String(a.get("kind", "asset")) : target.replace_char('\\', '/').simplify_path());
				accesses.push_back(project);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);

	_add_observe_exposed("asset.capabilities", "List compatible operations from every registered Solers plugin for a project asset. asset_id accepts a job id or a res:// .solers.json sidecar path.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Job id or res:// .solers.json sidecar path."}},"required":["asset_id"],"additionalProperties":false})", SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) { return svc->capabilities(a); }, {}, {}, {}, SolersToolUiKind::ASSET, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	Dictionary operation_schema = JSON::parse_string(R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Source job id or res:// .solers.json sidecar path."},"provider":{"type":"string"},"operation_id":{"type":"string","minLength":1},"options":{"type":"object"},"raw_provider_options":{"type":"object","description":"Advanced plugin-native options. Requires raw_confirmed=true."},"raw_confirmed":{"type":"boolean"},"target_dir":{"type":"string","description":"Optional res:// destination for the derived asset."},"import_profile":{"type":"string","enum":["runtime","baked_static"]},"max_triangles":{"type":"integer","minimum":0},"map_types":{"type":"array","items":{"type":"string"},"uniqueItems":true}},"required":["asset_id","provider","operation_id"],"additionalProperties":false})");
	Dictionary operation_properties = operation_schema["properties"];
	Dictionary operation_provider = operation_properties["provider"];
	operation_provider["enum"] = operation_plugin_ids;
	operation_provider["description"] = "Registered provider returned with the selected asset.capabilities operation.";
	operation_properties["provider"] = operation_provider;
	operation_schema["properties"] = operation_properties;
	const CharString operation_json = JSON::stringify(operation_schema).utf8();
	_add("asset.run_operation", "Run one provider-qualified operation advertised by asset.capabilities and import the derived result directly into the project. The source may be a current job id or a res:// .solers.json sidecar.", operation_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::OPERATION, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->run_operation_for_session(a, ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary source;
				source["mode"] = "read";
				source["key"] = "asset:" + String(a.get("asset_id", String()));
				accesses.push_back(source);
				Dictionary project;
				project["mode"] = "write";
				const String target = String(a.get("target_dir", String())).strip_edges();
				project["key"] = target.is_empty() ? String("*") : "project:" + target.replace_char('\\', '/').simplify_path();
				accesses.push_back(project);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);
	_add_observe_exposed("asset.status", "Read one asset job that has already reached a project-import terminal state (imported, draft, failed, cancelled, or interrupted). Returns ASSET_NOT_READY while the job is still processing — do not retry this call to poll progress; call job.wait once and stop issuing tools so Solers can park and resume this turn.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Stable id returned by an asset job."}},"required":["asset_id"],"additionalProperties":false})", SolersToolExposure::OPERATION, [svc](const SolersToolContext &, const Dictionary &a) { return svc->status(a); }, _access_by_arg("read", "asset:", "asset_id"), {}, {}, SolersToolUiKind::ASSET, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add_observe_exposed("job.wait", "Declare background asset jobs required before the Agent can continue. When no conflict-free work remains, call once and stop issuing tools; Solers parks this turn and resumes it after a requested job reaches its project-import terminal state.", R"({"type":"object","properties":{"ids":{"type":"array","minItems":1,"items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["ids"],"additionalProperties":false})", SolersToolExposure::OPERATION, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->wait_jobs(a, ctx.session_id); }, [](const Dictionary &a) {
				Array accesses;
				const Array ids = a.get("ids", Array());
				for (int i = 0; i < ids.size(); i++) {
					Dictionary access;
					access["mode"] = "read";
					access["key"] = "asset:" + String(ids[i]);
					accesses.push_back(access);
				}
				return accesses; }, {}, {}, SolersToolUiKind::THINK, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);
}
void SolersToolRegistry::_register_addon_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *service = asset_service;
	_add("addon.search", "Search installable Godot addons. Verified Solers bundles are ranked first; remaining results come from the official Godot Asset Library.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Plugin name or capability."},"limit":{"type":"integer","minimum":1,"maximum":50,"description":"Maximum results. Default 20."}},"required":["query"]})", SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::OPERATION, [service](const SolersToolContext &ctx, const Dictionary &args) { return service->addon_search(args, ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "addon-catalog:";
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, {}, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add("addon.inspect", "Inspect one exact Godot addon before installation. Returns inert package facts plus an optional bounded, data-only Agent Contract; repeated identical contracts are returned by id without reinjecting their full content.", R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1,"description":"Exact package plugin_id returned by addon.search."},"refresh":{"type":"boolean","description":"Redownload Asset Library metadata and archive instead of reusing the inert cache."}},"required":["source","plugin_id"]})", SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationDomain::NONE, Vector<String>(), SolersToolExposure::OPERATION, [this, service](const SolersToolContext &ctx, const Dictionary &args) { return _compact_addon_contract(ctx, service->addon_inspect(args, ctx.cancel_requested)); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "addon-cache:" + String(args.get("source", String())) + ":" + String(args.get("plugin_id", String()));
				accesses.push_back(access);
				return accesses; }, {}, {}, {}, [](const Dictionary &args) { return SolersAssetService::is_trusted_addon(args) ? SolersPermissionManager::PERMISSION_OBSERVE : SolersPermissionManager::PERMISSION_NETWORK; }, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add_observe_exposed("addon.list", "List Godot addons installed through Solers, including pinned version, source, package hash, enabled state, registered ClassDB types, missing files, restart requirements, and load errors.", R"({"type":"object","properties":{}})", SolersToolExposure::OPERATION, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_list(args); }, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(access);
				return accesses; }, {}, {}, SolersToolUiKind::OBSERVE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY);
	_add("addon.ensure", "Install and enable one inspected exact Godot addon version. Completes after the editor filesystem scan has registered the addon's classes; success means files exist, extensions are loaded, editor plugins are enabled, and all Contract entry classes are registered.", R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1},"version":{"type":"string","minLength":1},"sha256":{"type":"string","pattern":"^[0-9a-fA-F]{64}$"}},"required":["source","plugin_id","version","sha256"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_INSTALL_PLUGIN, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::OPERATION, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "project:res://addons/" + String(args.get("plugin_id", String())).to_lower();
				accesses.push_back(access);
				Dictionary lock;
				lock["mode"] = "write";
				lock["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(lock);
				return accesses; }, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure_finalize(args); }, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure_ready(args); }, {}, [](const Dictionary &args) { return SolersAssetService::is_trusted_addon(args) ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_INSTALL_PLUGIN; }, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY);
}

void SolersToolRegistry::_register_skill_tools() {
	_add_observe_exposed("skill.read", "Read one built-in Solers skill by exact name. Skills teach how to use existing native tools; they do not execute work.", R"({"type":"object","properties":{"name":{"type":"string","description":"Built-in skill name from the system skill catalog."}},"required":["name"]})", SolersToolExposure::OPERATION, [this](const SolersToolContext &, const Dictionary &a) {
				const String name = String(a.get("name", String())).strip_edges();
				if (name.is_empty()) {
					return _error("INVALID_ARGUMENT", "name is required.");
				}
				SolersBuiltinSkillView skill;
				if (!SolersBuiltinSkills::find_by_name(name, skill)) {
					return _error("UNKNOWN_SKILL", vformat("Unknown built-in skill: %s", name));
				}
				Dictionary data;
				data["name"] = skill.name;
				data["description"] = skill.description;
				data["content"] = skill.content;
				return _ok(data); }, {}, {}, {}, SolersToolUiKind::READ, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *ref = reflection_service;
	const SolersPermissionManager::Permission edit_scene = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	_add_observe_exposed("object.query", "Query the live edited scene, an exact project path, a Resource, an ObjectID, or native spatial facts between node pairs. target=path returns the current expected_state and applicable path operations.", R"({"type":"object","properties":{"target":{"type":"string","enum":["scene","path","resource","object","relations"]},"include_selection":{"type":"boolean"},"node_paths":{"type":"array","items":{"type":"string"},"uniqueItems":true,"minItems":1,"maxItems":64},"path_prefix":{"type":"string"},"name_contains":{"type":"string"},"class_name":{"type":"string"},"script_path":{"type":"string","pattern":"^res://"},"cursor":{"type":"integer","minimum":0},"max_results":{"type":"integer","minimum":1},"include_connections":{"type":"boolean"},"path":{"type":"string","pattern":"^res://"},"type_hint":{"type":"string"},"include_dependencies":{"type":"boolean"},"max_dependencies":{"type":"integer","minimum":0,"maximum":2048},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128},"methods":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":32},"object_id":{"type":"string","pattern":"^-?[0-9]+$"},"relations":{"type":"array","minItems":1,"maxItems":128,"items":{"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},"required":["a","b"],"additionalProperties":false}}},"required":["target"],"additionalProperties":false})", SolersToolExposure::OPERATION, [this, ref](const SolersToolContext &ctx, const Dictionary &a) {
				const String target = a.get("target", String());
				if (target == "relations") {
					Dictionary args = a.duplicate(true);
					args.erase("target");
					return ref->measure_spatial_relations(args);
				}
				if (target == "path") {
					if (!file_checkpoint) {
						return _error("PATH_QUERY_UNAVAILABLE", "Project path state is unavailable.", false);
					}
					Dictionary state = _solers_resource_state_receipt(file_checkpoint, a.get("path", String()));
					state["expected_state"] = Dictionary({ { "resources", Array({ state.duplicate(true) }) } });
					state["operations"] = _operation_definitions(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, StringName(), SNAME("path"));
					return _ok(state);
				}
				if (target == "resource") {
					if (!resource_service) {
						return _error("RESOURCE_QUERY_UNAVAILABLE", "Resource queries are unavailable.", false);
					}
					Dictionary args = a.duplicate(true);
					args.erase("target");
					Dictionary result = resource_service->inspect_resource(args);
					if ((bool)result.get("ok", false)) {
						Dictionary data = result.get("data", Dictionary());
						data["operations"] = _operation_definitions(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, StringName(data.get("resource_type", String())));
						result["data"] = data;
					}
					return result;
				}
				if (target == "object") {
					if (!resource_service) {
						return _error("OBJECT_QUERY_UNAVAILABLE", "ObjectID queries are unavailable.", false);
					}
					Dictionary args;
					args["object_id"] = a.get("object_id", Variant());
					Dictionary listed = resource_service->native_list_properties(args);
					if (!(bool)listed.get("ok", false)) {
						return listed;
					}
					Dictionary data = listed.get("data", Dictionary());
					const Array requested = a.get("properties", Array());
					if (!requested.is_empty()) {
						Dictionary values;
						Dictionary errors;
						for (int i = 0; i < requested.size(); i++) {
							args["property"] = requested[i];
							const Dictionary value = resource_service->native_get(args);
							if (!(bool)value.get("ok", false)) {
								errors[requested[i]] = value.get("error", Dictionary());
								continue;
							}
							values[requested[i]] = Dictionary(value.get("data", Dictionary())).get("value", Variant());
						}
						data["values"] = values;
						if (!errors.is_empty()) {
							data["property_errors"] = errors;
						}
					}
					Dictionary method_values;
					for (const Variant &method : Array(a.get("methods", Array()))) {
						args["method"] = method;
						const Dictionary value = resource_service->native_get(args);
						method_values[method] = (bool)value.get("ok", false) ? Dictionary(value.get("data", Dictionary())).get("value", Variant()) : value.get("error", Dictionary());
					}
					if (!method_values.is_empty()) {
						data["method_values"] = method_values;
					}
					const Dictionary object = data.get("object", Dictionary());
					data["operations"] = _operation_definitions(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, StringName(object.get("class_name", String())));
					return _ok(data);
				}

				Dictionary data;
				Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
				if (!edited_root) {
					return _error("NO_EDITED_SCENE", "Open a scene before querying target=scene.", true);
				}
				const Dictionary queried = observation_service->query_scene_nodes(a, ctx.result_token_budget);
				const Array queried_nodes = queried.get("nodes", Array());
				Array node_paths;
				for (int i = 0; i < queried_nodes.size(); i++) {
					node_paths.push_back(Dictionary(queried_nodes[i]).get("node_path", String()));
				}
				if (observation_service && (bool)a.get("include_selection", false)) {
					data["selection"] = observation_service->get_selection();
				}
				if (!node_paths.is_empty()) {
					Dictionary inspect_args = a.duplicate(true);
					inspect_args["node_paths"] = node_paths;
					const Dictionary inspected = ref->inspect_nodes(inspect_args);
					if (!(bool)inspected.get("ok", false)) {
						return inspected;
					}
					data.merge(inspected.get("data", Dictionary()), true);
					Array nodes = data.get("nodes", Array());
					Dictionary operation_definitions;
					for (int i = 0; i < nodes.size(); i++) {
						Dictionary node = nodes[i];
						const Array operations = _operation_names(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, StringName(node.get("class_name", String())));
						node["operations"] = operations;
						for (const Variant &operation : operations) {
							const StringName name = operation;
							operation_definitions[name] = _tool_to_dictionary(tools[name]);
						}
						nodes[i] = node;
					}
					data["nodes"] = nodes;
					data["operation_definitions"] = operation_definitions;
				} else {
					data["nodes"] = queried_nodes;
					data["count"] = 0;
				}
				data["query_errors"] = queried.get("errors", Array());
				data["cursor"] = queried.get("cursor", 0);
				if (queried.has("next_cursor")) {
					data["next_cursor"] = queried["next_cursor"];
				}
				if (queried_nodes.is_empty() && !Array(queried.get("errors", Array())).is_empty()) {
					Dictionary result = _error("NODE_QUERY_FAILED", "No requested live scene node exists.");
					result["data"] = data;
					return result;
				}
				data["state"] = _solers_scene_state_receipt();
				return _ok(data); }, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				const String target = a.get("target", String());
				if (target == "object") {
					access["key"] = "engine-object:" + String(a.get("object_id", Variant()));
				} else if (target == "relations" || target == "scene") {
					access["key"] = "scene:";
				} else {
					access["key"] = "project:" + String(a.get("path", String()));
				}
				accesses.push_back(access);
				return accesses; }, {}, {}, SolersToolUiKind::SCENE, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.create", "Create an instantiable Godot Node through the current scene UndoRedo action.", R"({"type":"object","properties":{"class_name":{"type":"string","minLength":1},"name":{"type":"string"},"parent_path":{"type":"string"},"properties":{"type":"object"}},"required":["class_name"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->create_node(a); }, _access_by_arg("write", "scene:", "parent_path"), SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.instance.instantiate", "Instantiate a PackedScene through the current scene UndoRedo action.", R"({"type":"object","properties":{"source_path":{"type":"string","pattern":"^res://"},"parent_path":{"type":"string"},"name":{"type":"string"},"properties":{"type":"object"}},"required":["source_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->instantiate_scene(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("read", "project:", "source_path")(a);
			accesses.append_array(_access_by_arg("write", "scene:", "parent_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.update", "Set typed Godot properties through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"properties":{"type":"object","minProperties":1}},"required":["node_path","properties"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->update_node(a); }, _access_by_arg("write", "scene:", "node_path"), SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.reparent", "Reparent a live Node through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"new_parent_path":{"type":"string","minLength":1},"position":{"type":"integer"}},"required":["node_path","new_parent_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->reparent_node(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("write", "scene:", "node_path")(a);
			accesses.append_array(_access_by_arg("write", "scene:", "new_parent_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.signal.connect", "Connect a live Godot signal through the current scene UndoRedo action.", R"({"type":"object","properties":{"source_path":{"type":"string","minLength":1},"signal":{"type":"string","minLength":1},"target_path":{"type":"string","minLength":1},"method":{"type":"string","minLength":1},"flags":{"type":"integer"}},"required":["source_path","signal","target_path","method"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->connect_signal(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("write", "scene:", "source_path")(a);
			accesses.append_array(_access_by_arg("write", "scene:", "target_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.script.attach", "Attach a loaded Script resource through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"script_path":{"type":"string","pattern":"^res://"}},"required":["node_path","script_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->attach_script(a); }, [](const Dictionary &a) {
			Array accesses = _access_by_arg("write", "scene:", "node_path")(a);
			accesses.append_array(_access_by_arg("read", "project:", "script_path")(a));
			return accesses; }, SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.node.remove", "Remove a live Node through the current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1}},"required":["node_path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->remove_node(a); }, _access_by_arg("write", "scene:", "node_path"), SNAME("Node"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.csg.bake", "Bake a CSG root through Godot's native CSG API and current scene UndoRedo action.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"artifact":{"type":"string","enum":["mesh","collision"]},"hide_source":{"type":"boolean"}},"required":["node_path","artifact"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_csg(a); }, _access_by_arg("write", "scene:", "node_path"), SNAME("CSGShape3D"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.lightmap.bake", "Bake LightmapGI through its native BakeError lifecycle, one scene UndoRedo action, and a checkpointed output directory.", R"({"type":"object","properties":{"node_path":{"type":"string","minLength":1},"path":{"type":"string","pattern":"^res://.*\\.lmbake$"}},"required":["node_path","path"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR | SolersToolMutationDomain::FILES, [ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_lightmap(a); }, [](const Dictionary &a) { return _access_by_arg("write", "project:", "path")(Dictionary({ { "path", String(a.get("path", String())).get_base_dir() } })); }, SNAME("LightmapGI"));
	if (resource_service) {
		SolersResourceService *resources = resource_service;
		_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "resource.create", "Create and save an instantiable Resource using ClassDB, PropertyInfo, and ResourceSaver.", R"({"type":"object","properties":{"class_name":{"type":"string","minLength":1},"path":{"type":"string","pattern":"^res://"},"properties":{"type":"object"}},"required":["class_name","path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, [resources](const SolersToolContext &, const Dictionary &a) { return resources->create_resource(a); }, _access_by_arg("write", "project:", "path"));
		_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "resource.update", "Set typed Resource properties and persist them through ResourceSaver.", R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://"},"properties":{"type":"object","minProperties":1},"type_hint":{"type":"string"}},"required":["path","properties"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationDomain::FILES, [resources](const SolersToolContext &, const Dictionary &a) { return resources->set_resource_property(a); }, _access_by_arg("write", "project:", "path"), SNAME("Resource"));
	}
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "scene.open", "Open a res:// scene via EditorInterface and return its native history receipt.", R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://","description":"Saved editable PackedScene to open; imported scenes remain read-only resources."}},"required":["path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationDomain::IRREVERSIBLE, [ref](const SolersToolContext &, const Dictionary &a) { return ref->open_scene(a); }, _access_by_arg("read", "project:", "path"));
	_add_operation(SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, "mesh.unwrap_uv2", "Prepare UV2 through Godot's native Mesh API and commit one scene UndoRedo action after every surface verifies ARRAY_FORMAT_TEX_UV2.", R"({"type":"object","properties":{"node_paths":{"type":"array","minItems":1,"items":{"type":"string"}}},"required":["node_paths"],"additionalProperties":false})", edit_scene, SolersToolMutationDomain::EDITOR, [ref](const SolersToolContext &ctx, const Dictionary &a) { return ref->unwrap_uv2(a, ctx.call_id); }, [](const Dictionary &a) {
			Array accesses;
			for (const Variant &path : Array(a.get("node_paths", Array()))) {
				accesses.append_array(_access_by_arg("write", "scene:", "node_path")(Dictionary({ { "node_path", path } })));
			}
			return accesses; }, SNAME("MeshInstance3D"), [ref](const SolersToolContext &, const Dictionary &a) { return ref->poll_uv2_unwrap(a); }, [ref](const SolersToolContext &, const Dictionary &a) { return ref->is_uv2_unwrap_ready(a); }, [ref](const SolersToolContext &ctx, const Dictionary &, const Dictionary &result) {
				if (!(bool)result.get("ok", false)) {
					ref->cancel_uv2_unwrap(ctx.call_id);
				} });
	_add_observe_exposed("engine.describe", "Search ClassDB or inspect exact classes. member_query returns matching typed members and documentation.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Fuzzy class search."},"inherits":{"type":"string"},"max_results":{"type":"integer","minimum":1,"maximum":200},"classes":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"class_name":{"type":"string","minLength":1},"include_inherited":{"type":"boolean"},"member_query":{"type":"string","description":"Filter typed members/docs; omit for names only."},"cursor":{"type":"integer","minimum":0,"description":"Cursor returned by the previous page. Default 0."},"max_members":{"type":"integer","minimum":1,"maximum":256,"description":"Optional page size shared by methods, properties, signals, and constants."}},"required":["class_name"],"additionalProperties":false},"description":"Exact classes to introspect. Lean without member_query; expand with member_query."}},"additionalProperties":false})", SolersToolExposure::OPERATION, [this](const SolersToolContext &, const Dictionary &a) { return _inspect_engine(a); }, {}, {}, {}, SolersToolUiKind::SEARCH, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::EDITOR, SolersOperationMode::QUERY);
}

void SolersToolRegistry::_register_authority_tools() {
	auto add_authority = [this](const char *p_name, const char *p_description, SolersOperationDomain p_domain, SolersOperationMode p_mode, SolersToolUiKind p_ui_kind) {
		SolersToolCapability capability;
		capability.operation_domain = p_domain;
		capability.operation_mode = p_mode;
		capability.permission_resolver = [this, p_domain, p_mode](const Dictionary &a) {
			SolersTool *operation = _operation(p_domain, p_mode, StringName(a.get("operation", String())));
			if (!operation) {
				return SolersPermissionManager::PERMISSION_OBSERVE;
			}
			const SolersToolCapability &operation_capability = operation->capability();
			const Dictionary arguments = a.get("arguments", Dictionary());
			return operation_capability.permission_resolver ? operation_capability.permission_resolver(arguments) : operation_capability.permission;
		};
		capability.mutation_domain_resolver = [this, p_domain, p_mode](const Dictionary &a) { return _operation_domains(p_domain, p_mode, a); };
		capability.resource_access = [this, p_domain, p_mode](const Dictionary &a) { return _operation_resource_access(p_domain, p_mode, a); };
		capability.argument_validator = [this, p_domain, p_mode](const Dictionary &a) { return _validate_operation(p_domain, p_mode, a); };
		capability.execution_resolver = [this, p_domain, p_mode](const Dictionary &a) { return _operation_execution(p_domain, p_mode, a); };
		capability.execution_ready = [this, p_domain, p_mode](const Dictionary &a) {
			SolersTool *operation = _operation(p_domain, p_mode, StringName(a.get("operation", String())));
			if (!operation || !operation->capability().execution_ready) {
				return true;
			}
			return operation->capability().execution_ready(a.get("arguments", Dictionary()));
		};
		capability.ui_kind = p_ui_kind;
		const Dictionary schema = p_mode == SolersOperationMode::APPLY ? _schema(R"({"type":"object","properties":{"operation":{"type":"string","minLength":1},"expected_state":{"type":"object","description":"Pass the exact expected_state returned by the query that exposed this operation."},"arguments":{"type":"object"}},"required":["operation","expected_state","arguments"],"additionalProperties":false})") : _schema(R"({"type":"object","properties":{"operation":{"type":"string","minLength":1,"description":"Use catalog for a compact index; pass arguments.operation for one full contract."},"arguments":{"type":"object"}},"required":["operation","arguments"],"additionalProperties":false})");
		_register(memnew(SolersFunctionTool(StringName(String::utf8(p_name)), String::utf8(p_description), schema, SolersToolExposure::MODEL, capability, [this, p_domain, p_mode](const SolersToolContext &ctx, const Dictionary &a) { return _execute_operation(p_domain, p_mode, ctx, a); }, [this, p_domain, p_mode](const SolersToolContext &ctx, const Dictionary &a) { return _poll_operation(p_domain, p_mode, ctx, a); }, [this, p_domain, p_mode](const SolersToolContext &ctx, const Dictionary &a) { return _is_operation_ready(p_domain, p_mode, ctx, a); }, [this, p_domain, p_mode](const SolersToolContext &ctx, const Dictionary &a, const Dictionary &result) { _complete_operation(p_domain, p_mode, ctx, a, result); })));
	};
	add_authority("editor.query", "Query authoritative project, ClassDB, resource, scene, and delivery state. Use operation=catalog to inspect contracts.", SolersOperationDomain::EDITOR, SolersOperationMode::QUERY, SolersToolUiKind::SEARCH);
	add_authority("editor.apply", "Apply one operation returned by editor.query under its native receipt, permission, UndoRedo, and checkpoint contract.", SolersOperationDomain::EDITOR, SolersOperationMode::APPLY, SolersToolUiKind::SCENE);
	add_authority("runtime.query", "Query the active Godot debugger and runtime state. Use operation=catalog to inspect contracts.", SolersOperationDomain::RUNTIME, SolersOperationMode::QUERY, SolersToolUiKind::RUN);
	add_authority("runtime.apply", "Apply one runtime operation under its native epoch and debugger contract.", SolersOperationDomain::RUNTIME, SolersOperationMode::APPLY, SolersToolUiKind::RUN);
	add_authority("pipeline.query", "Query assets, addons, exports, and background jobs. Use operation=catalog to inspect contracts.", SolersOperationDomain::PIPELINE, SolersOperationMode::QUERY, SolersToolUiKind::ASSET);
	add_authority("pipeline.apply", "Apply one asset, addon, export, or background-job operation returned by pipeline.query.", SolersOperationDomain::PIPELINE, SolersOperationMode::APPLY, SolersToolUiKind::ASSET);
}

void SolersToolRegistry::register_tool(SolersTool *p_tool) {
	ERR_FAIL_NULL(p_tool);
	_register(p_tool);
	_rebuild_tool_catalog();
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_add("history.revert", "Revert the latest reversible Agent mutation when its native UndoRedo version or file hashes still match.", R"({"type":"object","properties":{"reversal_id":{"type":"string","minLength":1}},"required":["reversal_id"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_SCENE, SolersToolMutationDomain::IRREVERSIBLE, Vector<String>(), SolersToolExposure::OPERATION, [this](const SolersToolContext &ctx, const Dictionary &a) { return _revert_latest(ctx, a); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
			Array accesses;
			Dictionary access;
			access["mode"] = "write";
			access["key"] = "*";
			accesses.push_back(access);
			return accesses; }, {}, {}, {}, [this](const Dictionary &a) {
			const Dictionary *record = _find_reversal(String(a.get("reversal_id", String())));
			return record && _mutation_record_has_domain(*record, "files") ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_EDIT_SCENE; }, {}, SolersToolUiKind::DEFAULT, {}, StringName(), SolersOperationDomain::EDITOR, SolersOperationMode::APPLY);
	_register_skill_tools();
	_register_reflection_tools();
	_register_observation_tools();
	_register_script_tools();
	_register_runtime_tools();
	_register_asset_tools();
	_register_addon_tools();
	_register_authority_tools();
	_rebuild_tool_catalog();
}

Dictionary SolersToolRegistry::_tool_to_dictionary(const SolersTool *p_tool) const {
	Dictionary tool;
	tool["name"] = String(p_tool->name());
	tool["model_name"] = _make_model_tool_name(p_tool->name());
	tool["description"] = p_tool->description();
	const SolersToolCapability &cap = p_tool->capability();
	tool["permission"] = permission_manager ? permission_manager->get_permission_name(cap.permission) : "observe";
	tool["permission_dynamic"] = (bool)cap.permission_resolver;
	tool["mutation_domains"] = _mutation_domain_names(cap.mutation_domains);
	tool["mutation_domains_dynamic"] = (bool)cap.mutation_domain_resolver;
	if (cap.operation_domain != SolersOperationDomain::NONE) {
		tool["authority"] = _operation_domain_name(cap.operation_domain);
		tool["mode"] = _operation_mode_name(cap.operation_mode);
	}
	if (!cap.target_class.is_empty()) {
		tool["target_class"] = String(cap.target_class);
	}
	if (!cap.target_kind.is_empty()) {
		tool["target_kind"] = String(cap.target_kind);
	}
	tool["execution"] = cap.execution == SolersToolExecution::WORKER_THREAD ? "worker" : "main_thread";
	tool["execution_dynamic"] = (bool)cap.execution_resolver;
	tool["exposure"] = _exposure_name(p_tool->exposure());
	tool["ui_kind"] = _ui_kind_name(cap.ui_kind);
	tool["timeline_visible"] = cap.host.timeline_visible;
	tool["attachment_args"] = cap.host.attachment_args;
	tool["required_model_inputs"] = cap.host.required_model_inputs;
	tool["input_schema"] = p_tool->parameters_schema().duplicate(true);
	tool["output_schema"] = Dictionary({ { "type", "object" }, { "properties", Dictionary() } });
	return tool;
}

void SolersToolRegistry::_rebuild_tool_catalog() {
	tool_catalog.clear();
	tool_catalog_by_name.clear();
	tool_catalog_revision++;
	Vector<String> names;
	for (const KeyValue<StringName, SolersTool *> &E : tools) {
		names.push_back(String(E.key));
	}
	names.sort();
	for (int i = 0; i < names.size(); i++) {
		SolersTool *const *tool = tools.getptr(StringName(names[i]));
		if (tool && *tool) {
			const Dictionary definition = _tool_to_dictionary(*tool);
			tool_catalog.push_back(definition);
			tool_catalog_by_name[StringName(names[i])] = definition;
		}
	}
}

Array SolersToolRegistry::list_tools() const {
	return tool_catalog.duplicate();
}

Dictionary SolersToolRegistry::get_tool_definition(const StringName &p_name) const {
	const Dictionary *definition = tool_catalog_by_name.getptr(p_name);
	return definition ? *definition : Dictionary();
}

Dictionary SolersToolRegistry::get_tool_definition(const StringName &p_name, const Dictionary &p_args) const {
	Dictionary definition = get_tool_definition(p_name);
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool || (*tool)->exposure() != SolersToolExposure::MODEL) {
		return definition;
	}
	const SolersToolCapability &capability = (*tool)->capability();
	SolersTool *operation = _operation(capability.operation_domain, capability.operation_mode, StringName(p_args.get("operation", String())));
	if (!operation) {
		return definition;
	}
	const Dictionary operation_definition = get_tool_definition(operation->name());
	for (const StringName &key : { SNAME("timeline_visible"), SNAME("attachment_args"), SNAME("required_model_inputs"), SNAME("ui_kind") }) {
		definition[key] = operation_definition.get(key, Variant());
	}
	return definition;
}

bool SolersToolRegistry::is_execution_ready(const StringName &p_name, const Dictionary &p_args) const {
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool || !(*tool)->capability().execution_ready) {
		return true;
	}
	return (*tool)->capability().execution_ready(p_args);
}

String SolersToolRegistry::get_skill_catalog_prompt() const {
	return SolersBuiltinSkills::build_catalog_prompt();
}

String SolersToolRegistry::get_model_tool_name(const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool || (*tool)->exposure() != SolersToolExposure::MODEL) {
		return String();
	}
	return _make_model_tool_name(p_name);
}

StringName SolersToolRegistry::resolve_model_tool_name(const String &p_model_name) const {
	const StringName model_name = StringName(p_model_name);
	const StringName *canonical = model_name_index.getptr(model_name);
	if (canonical) {
		return *canonical;
	}
	SolersTool *const *tool = tools.getptr(model_name);
	if (tool && *tool && (*tool)->exposure() == SolersToolExposure::MODEL) {
		return model_name;
	}
	return StringName();
}

bool SolersToolRegistry::affects_scene_state(const StringName &p_name, const Dictionary &p_args) const {
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool) {
		return false;
	}
	const SolersToolCapability &capability = (*tool)->capability();
	const SolersPermissionManager::Permission permission = capability.permission_resolver ? capability.permission_resolver(p_args) : capability.permission;
	return permission == SolersPermissionManager::PERMISSION_EDIT_SCENE;
}

bool SolersToolRegistry::is_read_only(const StringName &p_name, const Dictionary &p_args) const {
	const Array accesses = resolve_resource_access(p_name, p_args);
	if (accesses.is_empty()) {
		return false;
	}
	for (int i = 0; i < accesses.size(); i++) {
		if (String(Dictionary(accesses[i]).get("mode", "write")) != "read") {
			return false;
		}
	}
	return true;
}

Array SolersToolRegistry::resolve_resource_access(const StringName &p_name, const Dictionary &p_args) const {
	Array accesses;
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool) {
		Dictionary access;
		access["mode"] = "write";
		access["key"] = "*";
		accesses.push_back(access);
		return accesses;
	}
	const SolersToolCapability &cap = (*tool)->capability();
	if (cap.resource_access) {
		accesses = cap.resource_access(normalize_tool_args(p_name, p_args));
	}
	if (accesses.is_empty()) {
		Dictionary access;
		access["mode"] = cap.mutation_domains == SolersToolMutationDomain::NONE ? "read" : "write";
		access["key"] = "*";
		accesses.push_back(access);
	}
	return accesses;
}

bool SolersToolRegistry::has_write_conflict(const Array &p_left, const Array &p_right) {
	for (int i = 0; i < p_left.size(); i++) {
		const Dictionary left = p_left[i];
		const String left_key = left.get("key", "*");
		for (int j = 0; j < p_right.size(); j++) {
			const Dictionary right = p_right[j];
			if (String(right.get("mode", "write")) != "write") {
				continue;
			}
			const String right_key = right.get("key", "*");
			if (left_key == "*" || right_key == "*" || left_key == right_key) {
				return true;
			}
		}
	}
	return false;
}

Dictionary SolersToolRegistry::normalize_tool_args(const StringName &p_name, const Dictionary &p_args) const {
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return p_args.duplicate(true);
	}
	return _normalize_tool_args(p_args, (*tool_ptr)->parameters_schema());
}

Dictionary SolersToolRegistry::redact_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const {
	Dictionary out = p_args.duplicate(true);
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return out;
	}
	const SolersToolCapability &cap = (*tool_ptr)->capability();
	if ((*tool_ptr)->exposure() == SolersToolExposure::MODEL) {
		SolersTool *operation = _operation(cap.operation_domain, cap.operation_mode, StringName(out.get("operation", String())));
		if (operation && out.has("arguments")) {
			out["arguments"] = redact_tool_args_for_audit(operation->name(), out.get("arguments", Dictionary()));
		}
	}
	for (int i = 0; i < cap.redact_args.size(); i++) {
		const String &key = cap.redact_args[i];
		if (out.has(key)) {
			out[key] = "<redacted>";
		}
	}
	return out;
}

Dictionary SolersToolRegistry::summarize_tool_args_for_audit(const StringName &p_name, const Dictionary &p_args) const {
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return _trace_args(p_args);
	}
	Dictionary normalized = normalize_tool_args(p_name, p_args);
	const SolersToolCapability &capability = (*tool_ptr)->capability();
	if ((*tool_ptr)->exposure() == SolersToolExposure::MODEL) {
		SolersTool *operation = _operation(capability.operation_domain, capability.operation_mode, StringName(normalized.get("operation", String())));
		if (operation && normalized.has("arguments")) {
			normalized["arguments"] = summarize_tool_args_for_audit(operation->name(), normalized.get("arguments", Dictionary()));
		}
	}
	return _trace_args(normalized, &capability);
}

String SolersToolRegistry::summarize_tool_result_for_audit(const Dictionary &p_result) const {
	return _trace_result(p_result);
}

Dictionary SolersToolRegistry::call_tool(const StringName &p_name, const Dictionary &p_args) {
	return call_tool_with_context(p_name, p_args, SolersToolContext());
}

Dictionary SolersToolRegistry::call_tool_with_context(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context) {
	SolersPreparedToolCall call;
	const Dictionary preparation_error = _prepare_tool_call(p_name, p_args, p_context, call);
	if (!preparation_error.is_empty()) {
		return preparation_error;
	}
	const Dictionary result = _finalize_prepared_result(call, _execute_prepared_tool(call));
	_complete_prepared_tool(call, result);
	return result;
}

// Converge out-of-bounds numeric arguments of read-only calls to the schema
// boundary instead of rejecting the whole call: for an observation, "as much
// as allowed" always dominates "nothing". Mutating calls keep strict
// validation because their arguments carry intent.
static void _solers_clamp_numeric_args_to_schema(Dictionary &r_args, const Dictionary &p_schema) {
	const Dictionary properties = p_schema.get("properties", Dictionary());
	for (const Variant *key = properties.next(nullptr); key; key = properties.next(key)) {
		if (!r_args.has(*key)) {
			continue;
		}
		const Variant value = r_args[*key];
		if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) {
			continue;
		}
		const Dictionary property_schema = properties[*key];
		double numeric = value;
		if (property_schema.has("minimum")) {
			numeric = MAX(numeric, (double)property_schema["minimum"]);
		}
		if (property_schema.has("maximum")) {
			numeric = MIN(numeric, (double)property_schema["maximum"]);
		}
		if (numeric != (double)value) {
			r_args[*key] = value.get_type() == Variant::INT ? Variant((int64_t)numeric) : Variant(numeric);
		}
	}
}

Dictionary SolersToolRegistry::_preflight_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, Dictionary &r_args) {
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return _tool_result_envelope(_error("TOOL_NOT_FOUND", vformat("Solers tool not found: %s", p_name), true), p_context.call_id);
	}
	SolersTool *tool = *tool_ptr;
	Dictionary args = normalize_tool_args(p_name, p_args);
	const SolersToolCapability &preflight_cap = tool->capability();
	const SolersToolMutationDomain resolved_domains = preflight_cap.mutation_domain_resolver ? preflight_cap.mutation_domain_resolver(args) : preflight_cap.mutation_domains;
	if (resolved_domains == SolersToolMutationDomain::NONE) {
		_solers_clamp_numeric_args_to_schema(args, tool->parameters_schema());
	}
	Dictionary schema_args = args.duplicate(true);
	schema_args.erase("approval_id");
	Array internal_keys;
	const Variant *schema_key = nullptr;
	while ((schema_key = schema_args.next(schema_key))) {
		if (String(*schema_key).begins_with("_")) {
			internal_keys.push_back(*schema_key);
		}
	}
	for (const Variant &key : internal_keys) {
		schema_args.erase(key);
	}
	String argument_error;
	if (!_validate_tool_schema_value(schema_args, tool->parameters_schema(), "parameters", argument_error)) {
		Dictionary invalid = _error("TOOL_ARGUMENT_INVALID", argument_error, true);
		SOLERS_TRACE("registry.preflight_rejected", vformat("%s %s", String(p_name), argument_error));
		if (action_timeline) {
			Dictionary rejected;
			rejected["tool"] = p_name;
			rejected["args"] = redact_tool_args_for_audit(p_name, args);
			rejected["result"] = invalid;
			action_timeline->record_event("tool_call_rejected", rejected);
		}
		return _tool_result_envelope(invalid, p_context.call_id);
	}
	if (preflight_cap.argument_validator) {
		const Dictionary invalid = preflight_cap.argument_validator(args);
		if (!invalid.is_empty()) {
			return _tool_result_envelope(invalid, p_context.call_id);
		}
	}
	r_args = args;
	return Dictionary();
}

Dictionary SolersToolRegistry::_prepare_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, SolersPreparedToolCall &r_call) {
	Dictionary args;
	const Dictionary preflight_error = _preflight_tool_call(p_name, p_args, p_context, args);
	if (!preflight_error.is_empty()) {
		return preflight_error;
	}
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	ERR_FAIL_NULL_V(tool_ptr, _tool_result_envelope(_error("TOOL_NOT_FOUND", vformat("Solers tool not found: %s", p_name), true), p_context.call_id));
	SolersTool *tool = *tool_ptr;
	const SolersToolCapability &cap = tool->capability();
	r_call.mutation_domains = cap.mutation_domain_resolver ? cap.mutation_domain_resolver(args) : cap.mutation_domains;
	SolersPermissionManager::Permission effective_permission = is_read_only(p_name, args) ? SolersPermissionManager::PERMISSION_OBSERVE : (cap.permission_resolver ? cap.permission_resolver(args) : cap.permission);

	const Dictionary timeline_args = redact_tool_args_for_audit(p_name, args);
	Dictionary timeline_payload;
	timeline_payload["tool"] = p_name;
	timeline_payload["args"] = timeline_args;
	timeline_payload["permission"] = permission_manager ? permission_manager->get_permission_name(effective_permission) : "observe";
	if (action_timeline) {
		action_timeline->record_event("tool_call_started", timeline_payload);
	}

	if (!permission_manager) {
		return _tool_result_envelope(_error("PERMISSION_MANAGER_UNAVAILABLE", "Solers permission manager is not initialized.", false), p_context.call_id);
	}

	const int approval_id = args.get("approval_id", 0);
	const bool has_approval = permission_manager->is_auto_approved(effective_permission) || permission_manager->consume_approval(approval_id, p_name);
	if (!has_approval) {
		SOLERS_TRACE("registry.approval_required", vformat("%s perm=%s", String(p_name), permission_manager->get_permission_name(effective_permission)));
		Dictionary approval_request = permission_manager->request_user_approval(p_name, timeline_args, effective_permission);
		Dictionary denied = _error("USER_APPROVAL_REQUIRED", vformat("Tool requires approval before execution: %s", p_name), true);
		Dictionary error = denied.get("error", Dictionary());
		error["approval_request"] = approval_request;
		error["approval_id"] = approval_request.get("id", 0);
		denied["error"] = error;
		if (action_timeline) {
			Dictionary denied_payload = timeline_payload;
			denied_payload["result"] = denied;
			denied_payload["approval_request"] = approval_request;
			action_timeline->record_event("tool_call_blocked", denied_payload);
		}
		return _tool_result_envelope(denied, p_context.call_id);
	}

	SolersToolContext ctx = p_context;
	if (ctx.call_id.is_empty()) {
		ctx.call_id = args.get("call_id", String());
	}
	ctx.approval_id = approval_id;
	Dictionary handler_args = args;
	handler_args.erase("approval_id");
	r_call.tool = tool;
	r_call.name = p_name;
	r_call.args = handler_args;
	r_call.timeline_payload = timeline_payload;
	r_call.context = ctx;
	r_call.execution = cap.execution_resolver ? cap.execution_resolver(handler_args) : cap.execution;
	return _prepare_reversal(r_call);
}

Dictionary SolersToolRegistry::_execute_prepared_tool(SolersPreparedToolCall &p_call) {
	ERR_FAIL_NULL_V(p_call.tool, _error("TOOL_NOT_FOUND", "Prepared Solers tool is unavailable.", false));
	SOLERS_TRACE("registry.execute_begin", vformat("%s args=%s", String(p_call.name), _trace_json(summarize_tool_args_for_audit(p_call.name, p_call.args), 420)));
	const Dictionary result = _tool_result_envelope(p_call.tool->execute(p_call.context, p_call.args), p_call.context.call_id);
	SOLERS_TRACE("registry.execute_end", vformat("%s %s", String(p_call.name), summarize_tool_result_for_audit(result)));
	return result;
}

Dictionary SolersToolRegistry::_poll_prepared_tool(SolersPreparedToolCall &p_call, const Dictionary &p_args) {
	ERR_FAIL_NULL_V(p_call.tool, _error("TOOL_NOT_FOUND", "Prepared Solers tool is unavailable.", false));
	SOLERS_TRACE("registry.poll_begin", vformat("%s args=%s", String(p_call.name), _trace_json(summarize_tool_args_for_audit(p_call.name, p_args), 420)));
	const Dictionary result = _tool_result_envelope(p_call.tool->poll(p_call.context, p_args), p_call.context.call_id);
	SOLERS_TRACE("registry.poll_end", vformat("%s %s", String(p_call.name), summarize_tool_result_for_audit(result)));
	return result;
}

bool SolersToolRegistry::_is_prepared_tool_ready(const SolersPreparedToolCall &p_call, const Dictionary &p_args) const {
	return p_call.tool && p_call.tool->is_continuation_ready(p_call.context, p_args);
}

void SolersToolRegistry::_complete_prepared_tool(const SolersPreparedToolCall &p_call, const Dictionary &p_result) {
	if (p_call.tool) {
		p_call.tool->complete(p_call.context, p_call.args, p_result);
	}
	if (action_timeline) {
		Dictionary completed_payload = p_call.timeline_payload;
		completed_payload["ok"] = p_result.get("ok", false);
		action_timeline->record_event("tool_call_completed", completed_payload);
	}
}

void SolersToolRegistry::clear_task_state(const String &p_session_id) {
	if (p_session_id.is_empty()) {
		return;
	}
	Vector<String> delivery_keys;
	const String delivery_prefix = p_session_id + ":";
	for (const String &key : delivered_addon_contracts) {
		if (key.begins_with(delivery_prefix)) {
			delivery_keys.push_back(key);
		}
	}
	for (const String &key : delivery_keys) {
		delivered_addon_contracts.erase(key);
	}
}

void SolersToolRegistry::restore_session_reversals(const String &p_session_id, const Array &p_records) {
	if (p_session_id.is_empty()) {
		return;
	}
	reversals_by_session.erase(p_session_id);
	Vector<Dictionary> restored;
	for (const Variant &item : p_records) {
		if (item.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary record = item;
		if (!String(record.get("id", String())).is_empty() && String(record.get("session_id", String())) == p_session_id) {
			restored.push_back(record.duplicate(true));
		}
	}
	if (!restored.is_empty()) {
		reversals_by_session[p_session_id] = restored;
	}
}
Dictionary SolersToolRegistry::preview_session_rewind(const String &p_session_id, uint64_t p_target_revision) const {
	const Vector<Dictionary> *stack = reversals_by_session.getptr(p_session_id);
	Array records;
	if (stack) {
		for (const Dictionary &record : *stack) {
			if ((uint64_t)(int64_t)record.get("session_revision", 0) > p_target_revision) {
				records.push_back(record.duplicate(true));
			}
		}
	}
	HashMap<int, int64_t> expected_history_versions;
	HashMap<String, Dictionary> expected_file_states;
	HashMap<StringName, Variant> expected_project_settings;
	HashSet<String> files;
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	for (int i = records.size() - 1; i >= 0; i--) {
		const Dictionary record = records[i];
		const bool editor_domain = _mutation_record_has_domain(record, "editor");
		const bool files_domain = _mutation_record_has_domain(record, "files");
		if (_mutation_record_has_domain(record, "irreversible")) {
			return _error("REWIND_IRREVERSIBLE_BOUNDARY", "An irreversible Agent mutation exists after this message.", false);
		}
		if (!editor_domain && !files_domain) {
			return _error("REWIND_DOMAIN_UNSUPPORTED", "The mutation receipt has no supported reversal domain.", false);
		}
		if (editor_domain) {
			const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const int64_t before = record.get("version_before", 0);
			const int64_t after = record.get("version_after", 0);
			const int64_t *expected = expected_history_versions.getptr(history_id);
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			if ((!expected && (!undo_redo || (int64_t)undo_redo->get_version() != after)) || (expected && *expected != after)) {
				return _error("REWIND_NATIVE_HISTORY_CONFLICT", "Godot's UndoRedo history changed after the selected message.", false);
			}
			if (!expected) {
				const Dictionary expected_scene = Dictionary(record.get("receipt", Dictionary())).get("scene_after", Dictionary());
				const Dictionary current_scene = _solers_scene_state_receipt();
				for (const char *field : { "root_object_id", "scene_path", "history_id", "version" }) {
					if (expected_scene.has(field) && current_scene.get(field, Variant()) != expected_scene[field]) {
						return _error("REWIND_SCENE_CONFLICT", vformat("Godot scene fact changed: %s", field), false);
					}
				}
				const String scene_path = expected_scene.get("scene_path", String());
				if (!scene_path.is_empty()) {
					files.insert(scene_path);
				}
				const Dictionary *projected_file = expected_file_states.getptr(scene_path);
				const String expected_sha = expected_scene.get("saved_sha256", String());
				const String observed_sha = projected_file ? String(projected_file->get("sha256", String())) : String(current_scene.get("saved_sha256", String()));
				if (!expected_sha.is_empty() && observed_sha != expected_sha) {
					return _error("REWIND_SCENE_CONFLICT", "The saved scene SHA-256 changed after the selected message.", false);
				}
			}
			expected_history_versions[history_id] = before;
		}
		const Array checkpoints = files_domain ? Array(record.get("checkpoints", Array())) : Array();
		for (const Variant &item : checkpoints) {
			const Dictionary checkpoint = item;
			const String path = checkpoint.get("path", String());
			const bool after_exists = checkpoint.get("exists_after", false);
			const String after_sha = checkpoint.get("sha256_after", String());
			const Dictionary *expected = expected_file_states.getptr(path);
			if (expected) {
				if ((bool)expected->get("exists", false) != after_exists || (after_exists && String(expected->get("sha256", String())) != after_sha)) {
					return _error("REWIND_CHECKPOINT_CHAIN_BROKEN", vformat("The recorded file history is not contiguous: %s", path), false);
				}
			} else {
				const Dictionary current = _solers_checkpoint_target_state(file_checkpoint, path);
				if ((bool)current.get("existed", false) != after_exists || (after_exists && String(current.get("content_sha256", String())) != after_sha)) {
					return _error("REWIND_FILE_CONFLICT", vformat("File changed outside the recorded Agent history: %s", path), false);
				}
			}
			const String checkpoint_path = checkpoint.get("checkpoint_path", String());
			if ((bool)checkpoint.get("existed", false) && ((bool)checkpoint.get("directory", false) ? !DirAccess::exists(checkpoint_path) : !FileAccess::exists(checkpoint_path))) {
				return _error("REWIND_CHECKPOINT_MISSING", vformat("A required checkpoint is missing: %s", path), false);
			}
			const Dictionary settings_after = checkpoint.get("project_settings_after", Dictionary());
			for (const Variant *setting = settings_after.next(nullptr); setting; setting = settings_after.next(setting)) {
				const StringName name = *setting;
				const Variant *expected_setting = expected_project_settings.getptr(name);
				const Variant actual = expected_setting ? *expected_setting : ProjectSettings::get_singleton()->get(name);
				if (actual != settings_after[*setting]) {
					return _error("REWIND_PROJECT_SETTINGS_CONFLICT", vformat("Project setting changed after the Agent mutation: %s", name), false);
				}
			}
			Dictionary before_state;
			before_state["exists"] = checkpoint.get("existed", false);
			before_state["sha256"] = checkpoint.get("content_sha256", String());
			expected_file_states[path] = before_state;
			const Dictionary settings_before = checkpoint.get("project_settings", Dictionary());
			for (const Variant *setting = settings_before.next(nullptr); setting; setting = settings_before.next(setting)) {
				expected_project_settings[StringName(*setting)] = settings_before[*setting];
			}
			files.insert(path);
		}
	}
	Dictionary data;
	data["session_id"] = p_session_id;
	data["target_revision"] = (int64_t)p_target_revision;
	data["records"] = records;
	data["action_count"] = records.size();
	data["file_count"] = files.size();
	return _ok(data);
}

Dictionary SolersToolRegistry::prepare_session_rewind(const String &p_session_id, uint64_t p_target_revision) {
	const Dictionary preview = preview_session_rewind(p_session_id, p_target_revision);
	if (!(bool)preview.get("ok", false)) {
		return preview;
	}
	if (!file_checkpoint) {
		return _error("CHECKPOINT_SERVICE_UNAVAILABLE", "The file checkpoint service is not initialized.", false);
	}
	const Dictionary preview_data = preview.get("data", Dictionary());
	const Array records = preview_data.get("records", Array());
	HashSet<String> paths;
	for (const Variant &item : records) {
		const Dictionary record = item;
		if (_mutation_record_has_domain(record, "files")) {
			for (const Variant &checkpoint_item : Array(record.get("checkpoints", Array()))) {
				paths.insert(String(Dictionary(checkpoint_item).get("path", String())));
			}
		}
		if (_mutation_record_has_domain(record, "editor")) {
			const Dictionary scene_after = Dictionary(record.get("receipt", Dictionary())).get("scene_after", Dictionary());
			const String scene_path = scene_after.get("scene_path", String());
			if (!scene_path.is_empty()) {
				paths.insert(scene_path);
			}
		}
	}
	Array recovery;
	for (const String &path : paths) {
		const Dictionary checkpoint = file_checkpoint->create_checkpoint(path, "Solers historical-message rewind recovery");
		if (!(bool)checkpoint.get("ok", false)) {
			for (const Variant &created : recovery) {
				file_checkpoint->discard_checkpoint_state(created);
			}
			return checkpoint;
		}
		Dictionary recovery_state = checkpoint.get("data", Dictionary());
		Dictionary settings;
		for (const Variant &record_item : records) {
			for (const Variant &checkpoint_item : Array(Dictionary(record_item).get("checkpoints", Array()))) {
				const Dictionary recorded = checkpoint_item;
				if (String(recorded.get("path", String())) != path) {
					continue;
				}
				const Dictionary recorded_settings = recorded.get("project_settings", Dictionary());
				for (const Variant *setting = recorded_settings.next(nullptr); setting; setting = recorded_settings.next(setting)) {
					settings[*setting] = ProjectSettings::get_singleton()->get(*setting);
				}
			}
		}
		if (!settings.is_empty()) {
			recovery_state["project_settings"] = settings;
		}
		recovery.push_back(recovery_state);
	}
	Dictionary transaction = preview_data.duplicate(true);
	transaction["transaction_id"] = (p_session_id + ":" + String::num_uint64(p_target_revision) + ":" + String::num_uint64(Time::get_singleton()->get_ticks_usec())).sha256_text();
	transaction["recovery_checkpoints"] = recovery;
	return _ok(transaction);
}

Dictionary SolersToolRegistry::abort_session_rewind(const Dictionary &p_transaction) {
	const Array records = p_transaction.get("records", Array());
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	HashMap<int, int64_t> latest_history_versions;
	for (const Variant &item : records) {
		const Dictionary record = item;
		if (_mutation_record_has_domain(record, "editor")) {
			latest_history_versions[record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY)] = record.get("version_after", 0);
		}
	}
	for (const KeyValue<int, int64_t> &expected : latest_history_versions) {
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(expected.key) : nullptr;
		if (!undo_redo || (int64_t)undo_redo->get_version() > expected.value) {
			return _error("REWIND_COMPENSATION_CONFLICT", "Godot's UndoRedo history changed after the rewind transaction.", false);
		}
	}
	const Array recovery = p_transaction.get("recovery_checkpoints", Array());
	for (const Variant &recovery_item : recovery) {
		const Dictionary recovery_state = recovery_item;
		const String path = recovery_state.get("path", String());
		const Dictionary current = _solers_checkpoint_target_state(file_checkpoint, path);
		const bool exists = current.get("existed", false);
		const String sha = current.get("content_sha256", String());
		bool known = exists == (bool)recovery_state.get("existed", false) && (!exists || sha == String(recovery_state.get("content_sha256", String())));
		for (int i = records.size() - 1; i >= 0 && !known; i--) {
			const Dictionary record = records[i];
			if (_mutation_record_has_domain(record, "files")) {
				for (const Variant &checkpoint_item : Array(record.get("checkpoints", Array()))) {
					const Dictionary checkpoint = checkpoint_item;
					known = String(checkpoint.get("path", String())) == path && exists == (bool)checkpoint.get("existed", false) && (!exists || sha == String(checkpoint.get("content_sha256", String())));
					if (known) {
						break;
					}
				}
			}
			if (!known && _mutation_record_has_domain(record, "editor")) {
				const Dictionary receipt = record.get("receipt", Dictionary());
				for (const char *field : { "scene_before", "scene_after" }) {
					const Dictionary state = receipt.get(field, Dictionary());
					known = exists && String(state.get("scene_path", String())) == path && sha == String(state.get("saved_sha256", String()));
					if (known) {
						break;
					}
				}
			}
		}
		if (!known) {
			return _error("REWIND_COMPENSATION_CONFLICT", vformat("File changed outside the interrupted rewind: %s", path), false);
		}
	}
	bool scene_changed = false;
	for (const Variant &item : records) {
		const Dictionary record = item;
		if (!_mutation_record_has_domain(record, "editor")) {
			continue;
		}
		scene_changed = true;
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		const uint64_t before = (int64_t)record.get("version_before", 0);
		const uint64_t after = (int64_t)record.get("version_after", 0);
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
		if (!undo_redo) {
			return _error("REWIND_COMPENSATION_CONFLICT", "Godot's UndoRedo history is unavailable during compensation.", false);
		}
		const uint64_t current = undo_redo->get_version();
		if (current == before && (!manager->redo_history(history_id) || undo_redo->get_version() != after)) {
			return _error("REWIND_COMPENSATION_FAILED", "Godot could not redo a compensated editor action.", false);
		}
		if (current < before || (current > before && current < after)) {
			return _error("REWIND_COMPENSATION_CONFLICT", "Godot's UndoRedo history changed during compensation.", false);
		}
	}
	if (scene_changed && EditorInterface::get_singleton()) {
		EditorInterface::get_singleton()->save_scene();
	}
	for (int i = recovery.size() - 1; i >= 0; i--) {
		if (!file_checkpoint || !(bool)file_checkpoint->restore_checkpoint_state(recovery[i]).get("ok", false)) {
			return _error("REWIND_COMPENSATION_FAILED", "A recovery checkpoint could not be restored.", false);
		}
	}
	for (const Variant &checkpoint : recovery) {
		file_checkpoint->discard_checkpoint_state(checkpoint);
	}
	Dictionary data;
	data["transaction_id"] = p_transaction.get("transaction_id", String());
	data["compensated"] = true;
	return _ok(data);
}

Dictionary SolersToolRegistry::apply_session_rewind(const Dictionary &p_transaction) {
	const String session = p_transaction.get("session_id", String());
	const uint64_t target = (int64_t)p_transaction.get("target_revision", 0);
	const Dictionary checked = preview_session_rewind(session, target);
	if (!(bool)checked.get("ok", false)) {
		return checked;
	}
	const Array records = p_transaction.get("records", Array());
	const Array current_records = Dictionary(checked.get("data", Dictionary())).get("records", Array());
	if (records.size() > current_records.size()) {
		return _error("REWIND_PLAN_STALE", "The reversible mutation stack changed after confirmation.", false);
	}
	const int record_offset = current_records.size() - records.size();
	for (int i = 0; i < records.size(); i++) {
		if (String(Dictionary(records[i]).get("id", String())) != String(Dictionary(current_records[record_offset + i]).get("id", String()))) {
			return _error("REWIND_PLAN_STALE", "The reversible mutation stack changed after confirmation.", false);
		}
	}
	EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
	for (int i = records.size() - 1; i >= 0; i--) {
		const Dictionary record = records[i];
		bool applied = true;
		if (_mutation_record_has_domain(record, "editor")) {
			const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const uint64_t before = (int64_t)record.get("version_before", 0);
			const uint64_t after = (int64_t)record.get("version_after", 0);
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			applied = undo_redo && undo_redo->get_version() == after && manager->undo_history(history_id) && undo_redo->get_version() == before;
			if (applied && EditorInterface::get_singleton()) {
				EditorInterface::get_singleton()->save_scene();
				applied = !manager->is_history_unsaved(history_id);
			}
		}
		if (applied && _mutation_record_has_domain(record, "files")) {
			const Array checkpoints = record.get("checkpoints", Array());
			for (int checkpoint_index = checkpoints.size() - 1; checkpoint_index >= 0; checkpoint_index--) {
				const Dictionary checkpoint = checkpoints[checkpoint_index];
				applied = file_checkpoint && (bool)file_checkpoint->restore_checkpoint_state(checkpoint).get("ok", false) && _solers_checkpoint_matches(file_checkpoint, checkpoint, false);
				if (!applied) {
					break;
				}
			}
		}
		if (!applied) {
			return _error("REWIND_APPLY_FAILED", "The project rewind stopped before every native state could be restored.", false);
		}
	}
	Dictionary data = p_transaction.duplicate(true);
	data["applied"] = true;
	return _ok(data);
}
Dictionary SolersToolRegistry::finish_session_rewind(const Dictionary &p_transaction) {
	const String session = p_transaction.get("session_id", String());
	Vector<Dictionary> *stack = reversals_by_session.getptr(session);
	const Array records = p_transaction.get("records", Array());
	if (stack && records.size() <= stack->size()) {
		bool suffix_matches = true;
		const int stack_offset = stack->size() - records.size();
		for (int i = 0; i < records.size(); i++) {
			suffix_matches = suffix_matches && String(stack->get(stack_offset + i).get("id", String())) == String(Dictionary(records[i]).get("id", String()));
		}
		if (suffix_matches) {
			for (int i = records.size() - 1; i >= 0; i--) {
				_discard_reversal(stack->get(stack->size() - 1));
				stack->remove_at(stack->size() - 1);
			}
		}
	}
	for (const Variant &checkpoint : Array(p_transaction.get("recovery_checkpoints", Array()))) {
		if (file_checkpoint) {
			file_checkpoint->discard_checkpoint_state(checkpoint);
		}
	}
	Dictionary data;
	data["transaction_id"] = p_transaction.get("transaction_id", String());
	data["target_revision"] = p_transaction.get("target_revision", 0);
	return _ok(data);
}

int SolersToolRegistry::get_tool_count() const {
	return tools.size();
}

SolersToolRegistry::SolersToolRegistry() {}

SolersToolRegistry::~SolersToolRegistry() {
	_clear_tools();
	reversals_by_session.clear();
	delivered_addon_contracts.clear();
}
