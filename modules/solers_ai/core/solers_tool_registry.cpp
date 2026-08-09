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

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/fuzzy_search.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
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
		case SolersToolExposure::DIRECT:
			return "direct";
		case SolersToolExposure::DEFERRED:
			return "deferred";
		case SolersToolExposure::HIDDEN:
			return "hidden";
	}
	return "direct";
}

static const char *_mutation_policy_name(SolersToolMutationPolicy p_policy) {
	switch (p_policy) {
		case SolersToolMutationPolicy::READ_ONLY:
			return "read_only";
		case SolersToolMutationPolicy::EDITOR_UNDO:
			return "editor_undo";
		case SolersToolMutationPolicy::FILE_CHECKPOINT:
			return "file_checkpoint";
		case SolersToolMutationPolicy::IRREVERSIBLE:
			return "irreversible";
	}
	return "irreversible";
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
		return p_value.get_type() == Variant::INT;
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
		receipt["root_object_id"] = (int64_t)root->get_instance_id();
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

static Dictionary _solers_resource_state_receipt(const String &p_path) {
	Dictionary receipt;
	receipt["path"] = p_path;
	const bool exists = FileAccess::exists(p_path);
	receipt["exists"] = exists;
	if (exists) {
		receipt["sha256"] = FileAccess::get_sha256(p_path);
	}
	const ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_path);
	if (uid != ResourceUID::INVALID_ID) {
		receipt["resource_uid"] = ResourceUID::get_singleton()->id_to_text(uid);
	}
	return receipt;
}

static Array _solers_operation_targets(const Dictionary &p_data) {
	Array targets;
	const Array results = p_data.get("results", Array());
	for (int i = 0; i < results.size(); i++) {
		const Dictionary step = results[i];
		Dictionary target;
		for (const char *field : { "node_path", "object_id", "class_name", "native_facts", "path", "sha256" }) {
			if (step.has(field)) {
				target[field] = step[field];
			}
		}
		if (!target.is_empty()) {
			targets.push_back(target);
		}
	}
	return targets;
}

static bool _solers_scene_state_matches(const Dictionary &p_expected, const Dictionary &p_actual) {
	if ((int64_t)p_expected.get("history_id", -1) != (int64_t)p_actual.get("history_id", -2) ||
			(int64_t)p_expected.get("version", -1) != (int64_t)p_actual.get("version", -2)) {
		return false;
	}
	return !p_expected.has("root_object_id") || (int64_t)p_expected.get("root_object_id", 0) == (int64_t)p_actual.get("root_object_id", -1);
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
		if (data.has("completed") && !(bool)data.get("completed", true)) {
			const Array results = data.get("results", Array());
			if (!results.is_empty() && results[results.size() - 1].get_type() == Variant::DICTIONARY) {
				const Dictionary failed = results[results.size() - 1];
				const Dictionary failed_result = failed.get("result", Dictionary());
				const Dictionary error = failed_result.get("error", Dictionary());
				out += vformat(" completed=0 failed_op=%s failed_index=%d error=%s", String(failed.get("op", String())), (int)failed.get("index", -1), String(error.get("code", error.get("message", String()))));
			}
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

static void _append_search_text(String &r_corpus, const String &p_text) {
	if (!p_text.is_empty()) {
		r_corpus += " ";
		r_corpus += p_text.to_lower();
	}
}

static void _append_schema_search_text(const Variant &p_value, String &r_corpus) {
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = p_value;
		const Variant *key = nullptr;
		while ((key = dict.next(key))) {
			const String key_text = String(*key);
			const Variant value = dict[*key];
			if (key_text == "description" && value.get_type() == Variant::STRING) {
				_append_search_text(r_corpus, String(value));
			}
			if (key_text == "properties" && value.get_type() == Variant::DICTIONARY) {
				const Dictionary properties = value;
				const Variant *property_key = nullptr;
				while ((property_key = properties.next(property_key))) {
					_append_search_text(r_corpus, String(*property_key));
					_append_schema_search_text(properties[*property_key], r_corpus);
				}
			} else {
				_append_schema_search_text(value, r_corpus);
			}
		}
	} else if (p_value.get_type() == Variant::ARRAY) {
		const Array array = p_value;
		for (int i = 0; i < array.size(); i++) {
			_append_schema_search_text(array[i], r_corpus);
		}
	}
}

static String _tool_search_corpus(const SolersTool *p_tool) {
	String corpus;
	_append_search_text(corpus, String(p_tool->name()).replace(".", " "));
	_append_search_text(corpus, p_tool->description());
	_append_schema_search_text(p_tool->parameters_schema(), corpus);
	return corpus;
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
	const SolersToolMutationPolicy mutation_policy = p_tool->capability().mutation_policy;
	if ((mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO || mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT) && p_tool->capability().execution == SolersToolExecution::WORKER_THREAD) {
		ERR_PRINT(vformat("Reversible Solers tool '%s' must execute on the main thread.", name));
		memdelete(p_tool);
		return;
	}
	const StringName model_name = StringName(_make_model_tool_name(name));
	if (tools.has(name)) {
		ERR_PRINT(vformat("Solers tool already registered: %s", name));
		memdelete(p_tool);
		return;
	}
	if (model_name_index.has(model_name) && model_name_index[model_name] != name) {
		ERR_PRINT(vformat("Solers model tool name collision: %s maps to both %s and %s.", String(model_name), String(model_name_index[model_name]), String(name)));
		memdelete(p_tool);
		return;
	}
	tools[name] = p_tool;
	model_name_index[model_name] = name;
}

void SolersToolRegistry::_add(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersPermissionManager::Permission p_permission, SolersToolMutationPolicy p_mutation_policy,
		const Vector<String> &p_redact,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
		SolersToolExecution p_execution, std::function<Array(const Dictionary &)> p_resource_access,
		bool p_cache_across_revisions, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler, SolersFunctionTool::CompletionHandler p_completion_handler,
		std::function<SolersPermissionManager::Permission(const Dictionary &)> p_permission_resolver,
		std::function<SolersToolMutationPolicy(const Dictionary &)> p_mutation_policy_resolver,
		SolersToolUiKind p_ui_kind) {
	SolersToolCapability cap;
	cap.permission = p_permission;
	cap.permission_resolver = std::move(p_permission_resolver);
	cap.mutation_policy = p_mutation_policy;
	cap.mutation_policy_resolver = std::move(p_mutation_policy_resolver);
	cap.cache_across_revisions = p_cache_across_revisions;
	cap.execution = p_execution;
	cap.resource_access = std::move(p_resource_access);
	cap.redact_args = p_redact;
	cap.ui_kind = p_ui_kind;
	SolersTool *tool = memnew(SolersFunctionTool(StringName(String::utf8(p_name)), String::utf8(p_description),
			_schema(p_schema_json), p_exposure, cap, std::move(p_handler), std::move(p_poll_handler), std::move(p_ready_handler), std::move(p_completion_handler)));
	_register(tool);
}

void SolersToolRegistry::_add_observe_exposed(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
		std::function<Array(const Dictionary &)> p_resource_access, bool p_cache_across_revisions, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler,
		SolersToolUiKind p_ui_kind, SolersToolExecution p_execution) {
	_add(p_name, p_description, p_schema_json, SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationPolicy::READ_ONLY,
			Vector<String>(), p_exposure, std::move(p_handler),
			p_execution, std::move(p_resource_access), p_cache_across_revisions, std::move(p_poll_handler), std::move(p_ready_handler), {}, {}, {}, p_ui_kind);
}

void SolersToolRegistry::_add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler,
		SolersToolUiKind p_ui_kind) {
	_add_observe_exposed(p_name, p_description, p_schema_json, SolersToolExposure::DIRECT, std::move(p_handler), std::move(p_resource_access), false, std::move(p_poll_handler), std::move(p_ready_handler), p_ui_kind);
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
	if (r_call.mutation_policy == SolersToolMutationPolicy::READ_ONLY) {
		return Dictionary();
	}

	Dictionary state;
	state["policy"] = _mutation_policy_name(r_call.mutation_policy);
	if (r_call.mutation_policy == SolersToolMutationPolicy::IRREVERSIBLE) {
		state["scene_state_before"] = _solers_scene_state_receipt();
		r_call.reversal_state = state;
		return Dictionary();
	}
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO) {
		int history_id = EditorUndoRedoManager::INVALID_HISTORY;
		UndoRedo *undo_redo = _current_scene_undo_redo(history_id);
		if (!undo_redo) {
			return _tool_result_envelope(_error("UNDO_HISTORY_UNAVAILABLE", "The current edited scene has no UndoRedo history.", true), r_call.context.call_id);
		}
		state["history_id"] = history_id;
		state["version_before"] = (int64_t)undo_redo->get_version();
		state["scene_state_before"] = _solers_scene_state_receipt();
		r_call.reversal_state = state;
		return Dictionary();
	}

	if (!file_checkpoint) {
		return _tool_result_envelope(_error("CHECKPOINT_SERVICE_UNAVAILABLE", "The file checkpoint service is not initialized.", false), r_call.context.call_id);
	}
	Array checkpoints;
	Array resource_states_before;
	HashSet<String> seen_paths;
	const Array accesses = resolve_resource_access(r_call.name, r_call.args);
	for (int i = 0; i < accesses.size(); i++) {
		const Dictionary access = accesses[i];
		if (String(access.get("mode", "write")) != "write") {
			continue;
		}
		const String key = access.get("key", String());
		if (!key.begins_with("project:res://")) {
			continue;
		}
		const String path = key.trim_prefix("project:");
		if (seen_paths.has(path)) {
			continue;
		}
		seen_paths.insert(path);
		const Dictionary checkpoint = file_checkpoint->create_checkpoint(path, vformat("Solers tool %s", r_call.name));
		if (!(bool)checkpoint.get("ok", false)) {
			for (int checkpoint_index = 0; checkpoint_index < checkpoints.size(); checkpoint_index++) {
				file_checkpoint->discard_checkpoint_state(checkpoints[checkpoint_index]);
			}
			return _tool_result_envelope(checkpoint, r_call.context.call_id);
		}
		checkpoints.push_back(checkpoint.get("data", Dictionary()));
		resource_states_before.push_back(_solers_resource_state_receipt(path));
	}
	if (checkpoints.is_empty()) {
		return _tool_result_envelope(_error("CHECKPOINT_TARGET_UNDECLARED", vformat("Tool '%s' must declare concrete project file write targets.", r_call.name), false), r_call.context.call_id);
	}
	state["checkpoints"] = checkpoints;
	state["resource_states_before"] = resource_states_before;
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
	if (r_call.mutation_policy == SolersToolMutationPolicy::READ_ONLY) {
		return result;
	}

	auto rollback = [&]() -> bool {
		if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO) {
			const int history_id = r_call.reversal_state.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
			const uint64_t version_before = (int64_t)r_call.reversal_state.get("version_before", 0);
			EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
			UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
			while (manager && undo_redo && undo_redo->get_version() > version_before && manager->undo_history(history_id)) {
			}
			return undo_redo && undo_redo->get_version() == version_before;
		}
		if (r_call.mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT) {
			const Array checkpoints = r_call.reversal_state.get("checkpoints", Array());
			for (int i = checkpoints.size() - 1; i >= 0; i--) {
				const Dictionary checkpoint = checkpoints[i];
				const String path = checkpoint.get("path", String());
				const bool exists_now = FileAccess::exists(path);
				const bool unchanged = exists_now == (bool)checkpoint.get("existed", false) && (!exists_now || FileAccess::get_sha256(path) == String(checkpoint.get("content_sha256", String())));
				if (unchanged) {
					continue;
				}
				if (!file_checkpoint || !(bool)file_checkpoint->restore_checkpoint_state(checkpoint).get("ok", false)) {
					return false;
				}
			}
			_discard_reversal(r_call.reversal_state);
		}
		return true;
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
	if ((bool)data.get("checkpoint_consumed", false)) {
		r_call.journal_event["event_type"] = "checkpoint_consumed";
	}
	bool changed = (bool)data.get("authored_state_changed", false);
	Dictionary record = r_call.reversal_state.duplicate(true);
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO) {
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		const uint64_t version_before = (int64_t)record.get("version_before", 0);
		EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
		const uint64_t version_after = undo_redo ? undo_redo->get_version() : version_before;
		changed = version_after != version_before;
		if (changed && version_after != version_before + 1) {
			rollback();
			return _tool_result_envelope(_error("TOOL_UNDO_CONTRACT_VIOLATION", vformat("Tool '%s' must commit exactly one UndoRedo action.", r_call.name), false), r_call.context.call_id);
		}
		record["version_after"] = (int64_t)version_after;
	} else if (r_call.mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT) {
		Array checkpoints = record.get("checkpoints", Array());
		changed = false;
		for (int i = 0; i < checkpoints.size(); i++) {
			Dictionary checkpoint = checkpoints[i];
			const String path = checkpoint.get("path", String());
			const bool exists_after = FileAccess::exists(path);
			const String sha_after = exists_after ? FileAccess::get_sha256(path) : String();
			checkpoint["exists_after"] = exists_after;
			checkpoint["sha256_after"] = sha_after;
			changed = changed || exists_after != (bool)checkpoint.get("existed", false) || (exists_after && sha_after != String(checkpoint.get("content_sha256", String())));
			checkpoints[i] = checkpoint;
		}
		record["checkpoints"] = checkpoints;
	}

	if (!changed) {
		_discard_reversal(record);
		data["authored_state_changed"] = false;
		result["data"] = data;
		return result;
	}
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO && affects_scene_state(r_call.name, r_call.args)) {
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

	Dictionary receipt;
	receipt["call_id"] = r_call.context.call_id;
	receipt["policy"] = _mutation_policy_name(r_call.mutation_policy);
	const Array targets = _solers_operation_targets(data);
	if (!targets.is_empty()) {
		receipt["targets"] = targets;
	}
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO) {
		receipt["scene_before"] = r_call.reversal_state.get("scene_state_before", Dictionary());
		receipt["scene_after"] = _solers_scene_state_receipt();
		data.erase("results");
		data.erase("state_before");
		data.erase("state_after");
	} else if (r_call.mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT) {
		receipt["resources_before"] = r_call.reversal_state.get("resource_states_before", Array());
		Array resources_after;
		const Array checkpoints = record.get("checkpoints", Array());
		for (int i = 0; i < checkpoints.size(); i++) {
			resources_after.push_back(_solers_resource_state_receipt(Dictionary(checkpoints[i]).get("path", String())));
		}
		receipt["resources_after"] = resources_after;
	} else {
		receipt["scene_before"] = r_call.reversal_state.get("scene_state_before", Dictionary());
		receipt["scene_after"] = _solers_scene_state_receipt();
	}

	data["authored_state_changed"] = true;
	Dictionary mutation;
	mutation["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
	mutation["policy"] = _mutation_policy_name(r_call.mutation_policy);
	mutation["receipt"] = receipt;
	const String session_key = r_call.context.session_id.is_empty() ? String("direct") : r_call.context.session_id;
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO || r_call.mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT) {
		const String *previous_id = latest_reversal_by_session.getptr(session_key);
		if (previous_id) {
			const Dictionary *previous = reversals.getptr(*previous_id);
			if (previous) {
				_discard_reversal(*previous);
			}
			reversals.erase(*previous_id);
		}
		const String reversal_id = (session_key + ":" + r_call.context.call_id + ":" + String::num_uint64(r_call.context.authored_revision + 1) + ":" + String::num_int64(reversals.size() + 1)).sha256_text();
		record["id"] = reversal_id;
		record["session_id"] = session_key;
		record["session_revision"] = (int64_t)(r_call.context.authored_revision + 1);
		reversals[reversal_id] = record;
		latest_reversal_by_session[session_key] = reversal_id;
		mutation["reversal_id"] = reversal_id;
		r_call.journal_event["event_type"] = "checkpoint_created";
		r_call.journal_event["checkpoint"] = record;
		r_call.journal_event["note"] = "Protective checkpoint for history.revert; not a rollback of your edit.";
	} else {
		const String *previous_id = latest_reversal_by_session.getptr(session_key);
		if (previous_id) {
			const Dictionary *previous = reversals.getptr(*previous_id);
			if (previous) {
				_discard_reversal(*previous);
			}
			reversals.erase(*previous_id);
			latest_reversal_by_session.erase(session_key);
			r_call.journal_event["event_type"] = "checkpoint_cleared";
		}
	}
	data["mutation"] = mutation;
	result["data"] = data;
	return result;
}

Dictionary SolersToolRegistry::_revert_latest(const SolersToolContext &p_context, const Dictionary &p_args) {
	const String reversal_id = String(p_args.get("reversal_id", String())).strip_edges();
	const Dictionary *record_ptr = reversals.getptr(reversal_id);
	if (!record_ptr) {
		return _error("REVERSAL_NOT_FOUND", "The reversal id is unknown or has already been used.");
	}
	const Dictionary record = *record_ptr;
	const String session_key = p_context.session_id.is_empty() ? String("direct") : p_context.session_id;
	const String *latest = latest_reversal_by_session.getptr(session_key);
	if (!latest || *latest != reversal_id || String(record.get("session_id", String())) != session_key) {
		return _error("STALE_REVERSAL", "Only the latest Agent mutation at the current revision can be reverted.");
	}

	const String policy = record.get("policy", String());
	if (policy == "editor_undo") {
		const int history_id = record.get("history_id", EditorUndoRedoManager::INVALID_HISTORY);
		const uint64_t version_before = (int64_t)record.get("version_before", 0);
		const uint64_t version_after = (int64_t)record.get("version_after", 0);
		EditorUndoRedoManager *manager = EditorUndoRedoManager::get_singleton();
		UndoRedo *undo_redo = manager ? manager->get_history_undo_redo(history_id) : nullptr;
		if (!undo_redo || undo_redo->get_version() != version_after) {
			return _error("STALE_REVERSAL", "The editor UndoRedo history changed after this Agent mutation.");
		}
		if (!manager->undo_history(history_id) || undo_redo->get_version() != version_before) {
			return _error("REVERSAL_FAILED", "Godot could not restore the previous editor state.", false);
		}
	} else if (policy == "file_checkpoint") {
		const Array checkpoints = record.get("checkpoints", Array());
		for (int i = 0; i < checkpoints.size(); i++) {
			const Dictionary checkpoint = checkpoints[i];
			const String path = checkpoint.get("path", String());
			const bool exists_now = FileAccess::exists(path);
			if (exists_now != (bool)checkpoint.get("exists_after", false) || (exists_now && FileAccess::get_sha256(path) != String(checkpoint.get("sha256_after", String())))) {
				return _error("STALE_REVERSAL", vformat("File changed after the Agent mutation: %s", path));
			}
		}
		for (int i = checkpoints.size() - 1; i >= 0; i--) {
			const Dictionary restored = file_checkpoint ? file_checkpoint->restore_checkpoint_state(checkpoints[i]) : Dictionary();
			if (!(bool)restored.get("ok", false)) {
				return _error("REVERSAL_FAILED", "A file checkpoint could not be restored.", false);
			}
		}
	} else {
		return _error("REVERSAL_UNSUPPORTED", "The recorded mutation is not reversible.", false);
	}
	_discard_reversal(record);

	reversals.erase(reversal_id);
	latest_reversal_by_session.erase(session_key);
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
		return reflection_service->search_classes(search_args);
	}
	Array inspected_classes;
	for (const Variant &value : classes) {
		const Dictionary inspected = reflection_service->introspect_class(Dictionary(value));
		if (!(bool)inspected.get("ok", false)) {
			return inspected;
		}
		inspected_classes.push_back(inspected.get("data", Dictionary()));
	}
	Dictionary data;
	data["classes"] = inspected_classes;
	return _ok(data);
}

Dictionary SolersToolRegistry::_transact_objects(const Dictionary &p_args) {
	const String scope = String(p_args.get("scope", String())).strip_edges();
	if (scope == "scene") {
		if (!reflection_service) {
			return _error("SCENE_EDIT_UNAVAILABLE", "Scene transactions are unavailable.", false);
		}
		const Dictionary expected = p_args.get("expected_state", Dictionary());
		const Dictionary before = _solers_scene_state_receipt();
		if (!_solers_scene_state_matches(expected, before)) {
			Dictionary failure = _error("SCENE_STATE_CONFLICT", "The edited scene changed since it was inspected.");
			Dictionary data;
			data["expected_state"] = expected;
			data["actual_state"] = before;
			failure["data"] = data;
			return failure;
		}
		Dictionary result = reflection_service->batch(p_args);
		Dictionary data = result.get("data", Dictionary());
		data["scope"] = scope;
		data["state_before"] = before;
		if ((bool)result.get("ok", false)) {
			data["state_after"] = _solers_scene_state_receipt();
		}
		result["data"] = data;
		return result;
	}
	if (scope != "resource") {
		return _error("OBJECT_SCOPE_INVALID", "scope must be scene or resource.");
	}
	if (!resource_service) {
		return _error("RESOURCE_EDIT_UNAVAILABLE", "Resource transactions are unavailable.", false);
	}

	const Array operations = p_args.get("operations", Array());
	HashSet<String> paths;
	for (int i = 0; i < operations.size(); i++) {
		const Dictionary operation = operations[i];
		const String path = String(operation.get("path", String())).strip_edges();
		if (paths.has(path)) {
			return _error("RESOURCE_TARGET_DUPLICATE", vformat("A resource transaction may touch '%s' only once.", path));
		}
		paths.insert(path);
		if (String(operation.get("op", String())) == "update") {
			const Dictionary expected_state = operation.get("expected_state", Dictionary());
			const String expected = String(expected_state.get("sha256", String())).strip_edges();
			const String actual = FileAccess::exists(path) ? FileAccess::get_sha256(path) : String();
			if (String(expected_state.get("path", String())) != path || expected.is_empty() || expected != actual) {
				Dictionary failure = _error("RESOURCE_STATE_CONFLICT", vformat("Resource '%s' changed since it was inspected.", path));
				Dictionary data;
				data["path"] = path;
				data["expected_state"] = expected_state;
				Dictionary actual_state;
				actual_state["path"] = path;
				actual_state["sha256"] = actual;
				data["actual_state"] = actual_state;
				data["failed_index"] = i;
				failure["data"] = data;
				return failure;
			}
		}
	}

	Array results;
	for (int i = 0; i < operations.size(); i++) {
		const Dictionary operation = operations[i];
		const String action = String(operation.get("op", String())).strip_edges();
		Dictionary args = operation.duplicate(true);
		args["action"] = action;
		args.erase("op");
		args.erase("expected_state");
		const Dictionary result = resource_service->edit_resource(args);
		Dictionary step;
		step["index"] = i;
		step["op"] = action;
		step["result"] = result;
		results.push_back(step);
		if (!(bool)result.get("ok", false)) {
			Dictionary failure = _error("RESOURCE_TRANSACTION_FAILED", vformat("Resource transaction stopped at operation %d.", i));
			Dictionary data;
			data["results"] = results;
			data["failed_index"] = i;
			data["cause"] = result.get("error", Dictionary());
			failure["data"] = data;
			return failure;
		}
	}
	Array path_receipts;
	for (int i = 0; i < results.size(); i++) {
		const Dictionary step = results[i];
		Dictionary receipt;
		receipt["index"] = step.get("index", i);
		receipt["op"] = step.get("op", String());
		const Dictionary result_data = Dictionary(step.get("result", Dictionary())).get("data", Dictionary());
		const String path = result_data.get("path", String());
		if (!path.is_empty()) {
			receipt["path"] = path;
		}
		if (result_data.has("sha256")) {
			receipt["sha256"] = result_data["sha256"];
		} else if (result_data.has("content_sha256")) {
			receipt["sha256"] = result_data["content_sha256"];
		}
		path_receipts.push_back(receipt);
	}
	Dictionary data;
	data["scope"] = scope;
	data["results"] = path_receipts;
	data["count"] = path_receipts.size();
	data["ok_count"] = path_receipts.size();
	data["authored_state_changed"] = !path_receipts.is_empty();
	return _ok(data);
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

Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	ERR_FAIL_NULL_V(run_bar, _error("EDITOR_RUN_BAR_UNAVAILABLE", "The editor runtime controller is not available.", false));
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const String action = p_args.get("action", String());
	const bool was_playing = run_bar->is_playing();
	bool command_accepted = false;
	if (action == "set_property") {
		ERR_FAIL_NULL_V(observation_service, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		if (!debugger || !debugger->is_session_active()) {
			return _error("RUNTIME_NOT_CONNECTED", "Start the project before editing runtime state.");
		}
		if (observation_service->has_runtime_query()) {
			return _error("RUNTIME_QUERY_BUSY", "Wait for the active native runtime observation before changing runtime state.");
		}
		const uint64_t epoch = (int64_t)p_args.get("runtime_epoch", 0);
		const ObjectID object_id = ObjectID((uint64_t)(int64_t)p_args.get("object_id", 0));
		const StringName property = p_args.get("property", String());
		Variant before;
		if (!observation_service->get_runtime_property(epoch, object_id, property, before)) {
			return _error("STALE_RUNTIME_OBSERVATION", "Observe this object property in the current runtime epoch before changing it.");
		}
		if (before != p_args.get("expected_value", Variant())) {
			return _error("RUNTIME_PRECONDITION_FAILED", "The runtime property no longer matches expected_value.");
		}
		debugger->update_remote_object(object_id, property, p_args.get("value", Variant()));
		Dictionary query_args;
		query_args["target"] = "objects";
		Array object_ids;
		object_ids.push_back((int64_t)(uint64_t)object_id);
		query_args["object_ids"] = object_ids;
		Array properties;
		properties.push_back(property);
		query_args["properties"] = properties;
		Dictionary pending = observation_service->observe_runtime(query_args);
		if (pending.get("status", String()) != "pending") {
			return _error("RUNTIME_VERIFY_UNAVAILABLE", "The native debugger could not start post-write verification.");
		}
		Dictionary poll_args = pending.get("poll_args", Dictionary());
		poll_args["action"] = action;
		poll_args["before"] = before;
		poll_args["value"] = p_args.get("value", Variant());
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
	if (action == "set_property") {
		return !observation_service || observation_service->is_runtime_observation_ready(p_args);
	}
	return true;
}

Dictionary SolersToolRegistry::_poll_runtime_control(const Dictionary &p_args) const {
	const String action = p_args.get("action", String());
	if (action == "set_property") {
		ERR_FAIL_NULL_V(observation_service, _error("RUNTIME_OBSERVATION_UNAVAILABLE", "Runtime observation is not available.", false));
		Dictionary observed = observation_service->observe_runtime(p_args);
		if (observed.get("status", String()) == "pending") {
			return _ok(observed);
		}
		const Array objects = observed.get("objects", Array());
		if (objects.is_empty()) {
			return _error("RUNTIME_OBJECT_DISAPPEARED", "The runtime object disappeared before verification.");
		}
		const Dictionary object = objects[0];
		const StringName property = p_args.get("property", String());
		const Dictionary properties = object.get("properties", Dictionary());
		const Variant after = properties.get(property, Variant());
		if (!properties.has(property) || after != p_args.get("value", Variant())) {
			return _error("RUNTIME_WRITE_NOT_CONFIRMED", "The native debugger did not confirm the requested runtime value.");
		}
		Dictionary data;
		data["action"] = action;
		data["runtime_only"] = true;
		data["runtime_epoch"] = observed.get("runtime_epoch", 0);
		data["object_id"] = object.get("object_id", 0);
		data["property"] = property;
		data["before"] = p_args.get("before", Variant());
		data["after"] = after;
		data["target_state_confirmed"] = true;
		return _ok(data);
	}
	return _error("RUNTIME_CONTINUATION_INVALID", "Only runtime property verification has a continuation.", false);
}

void SolersToolRegistry::_register_observation_tools() {
	if (!observation_service) {
		return;
	}
	SolersObservationService *obs = observation_service;

	_add_observe_exposed("project.search", "Search project paths or text files on the worker. Inspect the live scene with object.query and disk Resources with target=resource.", R"({"type":"object","properties":{"type":{"type":"string","enum":["path","text","symbol"]},"query":{"type":"string","minLength":1,"description":"Case-insensitive path/text query."},"max_results":{"type":"integer","minimum":1,"maximum":256,"description":"Maximum results. Default 64."}},"required":["type","query"],"additionalProperties":false})", SolersToolExposure::DIRECT, [this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->search_project(a)); }, {}, false, {}, {}, SolersToolUiKind::SEARCH, SolersToolExecution::WORKER_THREAD);
	_add_observe("project.read_file", "Read a project text file from res://. PackedScene defaults to SCENE_TEXT_DENIED with a digest — that digest is the observation answer; do not retry with raw=true. Use object.query target=scene for scene facts. raw=true only when editing .tscn/.scn source.", R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to read."},"max_bytes":{"type":"integer","description":"Maximum bytes to return. Default 262144."},"raw":{"type":"boolean","description":"Only for PackedScene source editing. Default false — observation uses digest."}},"required":["path"]})", [this, obs](const SolersToolContext &, const Dictionary &a) {
				const Dictionary file = obs->read_project_file(a.get("path", String()), (int)a.get("max_bytes", 262144), (bool)a.get("raw", false));
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
				return _ok(file); }, _access_by_arg("read", "project:", "path"), {}, {}, SolersToolUiKind::READ);
	_add_observe("runtime.observe", "Observe the running game through Godot's native debugger. Query lifecycle events, the remote SceneTree, selected object properties, paused stack frames, or one performance sample.", R"({"type":"object","properties":{"target":{"type":"string","enum":["events","tree","objects","stack","performance"]},"since_cursor":{"type":"integer","minimum":0},"include_events":{"type":"boolean"},"max_events":{"type":"integer","minimum":0,"maximum":256},"object_ids":{"type":"array","items":{"type":"integer","minimum":1},"minItems":1,"maxItems":16,"uniqueItems":true},"properties":{"type":"array","items":{"type":"string","minLength":1},"maxItems":64,"uniqueItems":true},"max_results":{"type":"integer","minimum":1,"maximum":512}},"additionalProperties":false})", [this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->observe_runtime(a)); }, {}, [this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->observe_runtime(a)); }, [obs](const SolersToolContext &, const Dictionary &a) { return obs->is_runtime_observation_ready(a); });
	_add_observe_exposed("render.capture", "Capture content-addressed visual evidence from an explicit native state. Edited-scene captures require the history/version returned by object.query or object.transaction; the receipt binds the image to the exact World3D render-state fingerprint. debug_draw uses Godot's Viewport enum; inspect it with engine.describe.", R"({"type":"object","properties":{"target":{"type":"string","enum":["editor","camera","top_down","orthographic","runtime"]},"source_state":{"type":"object","properties":{"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0},"root_object_id":{"type":"integer"}},"required":["history_id","version"],"additionalProperties":true},"node_path":{"type":"string"},"axis":{"type":"string","enum":["x","y","z"]},"direction":{"type":"string","enum":["positive","negative"]},"focus_paths":{"type":"array","items":{"type":"string"}},"section_position":{"type":"number"},"debug_draw":{"type":"integer","minimum":0}},"required":["target"],"additionalProperties":false})", SolersToolExposure::DIRECT, [obs](const SolersToolContext &, const Dictionary &a) { return obs->capture_viewport(a); }, {}, false, [obs](const SolersToolContext &, const Dictionary &a) { return obs->poll_viewport_capture(a); }, [obs](const SolersToolContext &, const Dictionary &a) { return obs->is_viewport_capture_ready(a); }, SolersToolUiKind::CAPTURE);

	if (resource_service) {
		SolersResourceService *svc = resource_service;
		_add_observe_exposed("export.list_presets", "List Godot export platforms and export presets from the current project.",
				R"({"type":"object","properties":{"include_platforms":{"type":"boolean","description":"Include available export platforms. Default true."}}})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->list_export_presets(a); });
		_add_observe_exposed("export.validate_presets", "Validate configured export presets without exporting build artifacts.",
				R"({"type":"object","properties":{"debug":{"type":"boolean","description":"Validate against the debug export template. Default false."}}})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_export_presets(a); });
		_add("export.run_preset", "Run Godot's native EditorExportPlatform::export_project for one export preset.",
				R"({"type":"object","properties":{"preset_index":{"type":"integer","description":"Export preset index from export.list_presets."},"preset_name":{"type":"string","description":"Export preset name when index is unknown."},"debug":{"type":"boolean","description":"Export debug build. Default false."},"export_path":{"type":"string","description":"Optional output path override; defaults to the preset export_path."}}})",
				SolersPermissionManager::PERMISSION_EXPORT_BUILD, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_export_preset(a); });
	}
}

void SolersToolRegistry::_register_script_tools() {
	if (!script_service) {
		return;
	}
	SolersScriptService *svc = script_service;
	Vector<String> project_redact;
	project_redact.push_back("content");
	_add("project.edit", "Edit project settings through ProjectSettings, write an ordinary project data file, or create an empty directory. Raw writes to project.godot, scripts, scenes, resources, and import-pipeline formats are rejected.", R"({"type":"object","properties":{"operation":{"type":"string","enum":["settings","write_file","create_directory"]},"values":{"type":"object"},"erase":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"path":{"type":"string","pattern":"^res://"},"content":{"type":"string"}},"required":["operation"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::FILE_CHECKPOINT, project_redact, SolersToolExposure::DIRECT, [svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_project(a); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = String(a.get("operation", String())) == "settings" ? "project:res://project.godot" : "project:" + String(a.get("path", String()));
				accesses.push_back(access);
				return accesses; }, false, {}, {}, {}, {}, [](const Dictionary &a) { return String(a.get("operation", String())) == "settings" ? SolersToolMutationPolicy::EDITOR_UNDO : SolersToolMutationPolicy::FILE_CHECKPOINT; });

	Vector<String> script_redact;
	script_redact.push_back("content");
	script_redact.push_back("old_text");
	script_redact.push_back("new_text");
	_add("script.edit", "Create a script or replace one text block. old_text matches the current file content with whitespace/typography-tolerant fallbacks, so copy it from the latest read without re-deriving hashes. The write commits, is checkpointed (reversible via history.revert), and returns the parser's full diagnostics plus the patched region as it now exists on disk; fix reported errors with a follow-up edit.", R"({"type":"object","properties":{"operation":{"type":"string","enum":["create","replace"]},"path":{"type":"string","pattern":"^res://.*\\.(gd|cs|gdshader|gdshaderinc)$"},"content":{"type":"string"},"old_text":{"type":"string","minLength":1},"new_text":{"type":"string"},"occurrence":{"type":"integer","minimum":1}},"required":["operation","path"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::FILE_CHECKPOINT, script_redact, SolersToolExposure::DIRECT, [svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_script(a); }, SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
	_add_observe_exposed("script.validate", "Validate script source through Godot's registered ScriptLanguage implementation.", R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the script to validate."},"source":{"type":"string","description":"Optional source override; validates this text instead of the file content."}},"required":["path"]})", SolersToolExposure::DIRECT, [svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_script(a); }, {}, false, {}, {}, SolersToolUiKind::READ);
	Vector<String> compute_redact;
	compute_redact.push_back("source");
	_add("script.compute", "Run a complete SceneTree GDScript in an isolated temporary Godot project, then atomically commit only declared outputs. The script must quit its SceneTree; optional result data goes in res://result.json.", R"({"type":"object","properties":{"source":{"type":"string","minLength":1,"description":"Complete GDScript extending SceneTree. res:// is isolated; call quit() when finished."},"outputs":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","properties":{"from":{"type":"string","minLength":1,"description":"Relative path produced inside the isolated project."},"to":{"type":"string","pattern":"^res://","description":"Project destination committed through a file checkpoint."},"resource_type":{"type":"string","minLength":1,"description":"Optional Godot class required to reload after commit."}},"required":["from","to"],"additionalProperties":false}}},"required":["source","outputs"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::FILE_CHECKPOINT, compute_redact, SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->compute_script(ctx.call_id, a); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				const Array outputs = a.get("outputs", Array());
				for (int i = 0; i < outputs.size(); i++) {
					Dictionary access;
					access["mode"] = "write";
					access["key"] = "project:" + String(Dictionary(outputs[i]).get("to", String()));
					accesses.push_back(access);
				}
				return accesses; }, false, [svc](const SolersToolContext &ctx, const Dictionary &) { return svc->compute_script_finalize(ctx.call_id); }, [svc](const SolersToolContext &ctx, const Dictionary &) { return svc->compute_script_ready(ctx.call_id); }, [svc](const SolersToolContext &ctx, const Dictionary &, const Dictionary &) { svc->compute_script_complete(ctx.call_id); });
}

void SolersToolRegistry::_register_runtime_tools() {
	const SolersPermissionManager::Permission run_project = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	_add("runtime.control", "Control Godot's active debugger or make one preconditioned, runtime-only property change. Persist verified changes separately through object.transaction.", R"({"type":"object","properties":{"action":{"type":"string","enum":["play_current_scene","stop","suspend","resume","next_frame","debug_break","debug_continue","debug_step","debug_next","debug_out","set_property"]},"runtime_epoch":{"type":"integer","minimum":0},"object_id":{"type":"integer","minimum":1},"property":{"type":"string","minLength":1},"expected_value":{},"value":{}},"required":["action"],"additionalProperties":false})", run_project, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT, [this](const SolersToolContext &, const Dictionary &a) { return _run_control(a); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "runtime:";
				accesses.push_back(access);
				return accesses; }, false, [this](const SolersToolContext &, const Dictionary &a) { return _poll_runtime_control(a); }, [this](const SolersToolContext &, const Dictionary &a) { return _is_runtime_control_ready(a); });
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
	query_schema["minLength"] = 1;
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
	const CharString catalog_search_description = vformat("Search lightweight metadata through a registered catalog plugin (%s). Inspect a selected result before acquiring it.", catalog_labels).utf8();
	_add("asset.catalog.search", catalog_search_description.get_data(), catalog_search_json.get_data(), SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_search(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-directory:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower();
				accesses.push_back(access);
				return accesses; });

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
	_add("asset.catalog.inspect", "Resolve one exact catalog result into authoritative variants, dependencies, licensing, and checksums. asset.catalog.acquire accepts only a previously inspected variant.", catalog_inspect_json.get_data(), SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_inspect(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-detail:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower() + ":" + String(a.get("asset_id", String())).to_lower();
				accesses.push_back(access);
				return accesses; });

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
	const CharString generate_description = vformat("Generate an asset through a registered Solers plugin (%s), stage provider output under user://solers_jobs, then import it directly into the requested res:// project folder. The returned job becomes terminal only after Godot verifies the imported resources. For Meshy Image-to-3D hero quality use provider_options model_type=standard and ai_model=meshy-6 (or latest), optionally should_remesh=false; use smart-topology/meshy-t2 only when an explicit low-poly budget is required. Keep prompts short when input_attachments are set.", generation_labels).utf8();
	_add("asset.generate", generate_description.get_data(), generate_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->generate_for_session(_solers_apply_plugin_mention(ctx, a, "supports_generation"), ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
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
				return accesses; });

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
	_add("asset.catalog.acquire", "Acquire one exact inspected catalog variant, verify its source metadata and checksums, then import it directly into res:// and write project-local license/attribution metadata.", acquire_json.get_data(), SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_acquire(_solers_apply_plugin_mention(ctx, a, "supports_catalog"), ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
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
				return accesses; });

	_add_observe_exposed("asset.capabilities", "List operations exposed by the plugin that created a project asset. asset_id accepts a job id or a res:// .solers.json sidecar path.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Job id or res:// .solers.json sidecar path."}},"required":["asset_id"],"additionalProperties":false})", SolersToolExposure::DIRECT, [svc](const SolersToolContext &, const Dictionary &a) { return svc->capabilities(a); }, {}, false, {}, {}, SolersToolUiKind::ASSET);
	_add("asset.run_operation", "Run an operation advertised by asset.capabilities and import the derived result directly into the project. The source may be a current job id or a res:// .solers.json sidecar.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Source job id or res:// .solers.json sidecar path."},"operation_id":{"type":"string","minLength":1},"options":{"type":"object"},"raw_provider_options":{"type":"object","description":"Advanced plugin-native options. Requires raw_confirmed=true."},"raw_confirmed":{"type":"boolean"},"target_dir":{"type":"string","description":"Optional res:// destination for the derived asset."},"import_profile":{"type":"string","enum":["runtime","baked_static"]},"max_triangles":{"type":"integer","minimum":0},"map_types":{"type":"array","items":{"type":"string"},"uniqueItems":true}},"required":["asset_id","operation_id"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->run_operation_for_session(a, ctx.session_id); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
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
				return accesses; });
	_add_observe_exposed("asset.status", "Read one asset job that has already reached a project-import terminal state (imported, draft, failed, cancelled, or interrupted). Returns ASSET_NOT_READY while the job is still processing — do not retry this call to poll progress; call job.wait once and stop issuing tools so Solers can park and resume this turn.", R"({"type":"object","properties":{"asset_id":{"type":"string","minLength":1,"description":"Stable id returned by an asset job."}},"required":["asset_id"],"additionalProperties":false})", SolersToolExposure::DIRECT, [svc](const SolersToolContext &, const Dictionary &a) { return svc->status(a); }, _access_by_arg("read", "asset:", "asset_id"), false, {}, {}, SolersToolUiKind::ASSET);
	_add_observe_exposed("job.wait", "Declare background asset jobs required before the Agent can continue. When no conflict-free work remains, call once and stop issuing tools; Solers parks this turn and resumes it after a requested job reaches its project-import terminal state.", R"({"type":"object","properties":{"ids":{"type":"array","minItems":1,"items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["ids"],"additionalProperties":false})", SolersToolExposure::DIRECT, [svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->wait_jobs(a, ctx.session_id); }, [](const Dictionary &a) {
				Array accesses;
				const Array ids = a.get("ids", Array());
				for (int i = 0; i < ids.size(); i++) {
					Dictionary access;
					access["mode"] = "read";
					access["key"] = "asset:" + String(ids[i]);
					accesses.push_back(access);
				}
				return accesses; }, false, {}, {}, SolersToolUiKind::THINK);
}
void SolersToolRegistry::_register_addon_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *service = asset_service;
	_add("addon.search", "Search installable Godot addons. Verified Solers bundles are ranked first; remaining results come from the official Godot Asset Library.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Plugin name or capability."},"limit":{"type":"integer","minimum":1,"maximum":50,"description":"Maximum results. Default 20."}},"required":["query"]})", SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT, [service](const SolersToolContext &ctx, const Dictionary &args) { return service->addon_search(args, ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "addon-catalog:";
				accesses.push_back(access);
				return accesses; });
	_add("addon.inspect", "Inspect one exact Godot addon before installation. Returns inert package facts plus an optional bounded, data-only Agent Contract; repeated identical contracts are returned by id without reinjecting their full content.", R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1,"description":"Exact package plugin_id returned by addon.search."},"refresh":{"type":"boolean","description":"Redownload Asset Library metadata and archive instead of reusing the inert cache."}},"required":["source","plugin_id"]})", SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT, [this, service](const SolersToolContext &ctx, const Dictionary &args) { return _compact_addon_contract(ctx, service->addon_inspect(args, ctx.cancel_requested)); }, SolersToolExecution::WORKER_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "addon-cache:" + String(args.get("source", String())) + ":" + String(args.get("plugin_id", String()));
				accesses.push_back(access);
				return accesses; }, false, {}, {}, {}, [](const Dictionary &args) { return SolersAssetService::is_trusted_addon(args) ? SolersPermissionManager::PERMISSION_OBSERVE : SolersPermissionManager::PERMISSION_NETWORK; });
	_add_observe_exposed("addon.list", "List Godot addons installed through Solers, including pinned version, source, package hash, enabled state, registered ClassDB types, missing files, restart requirements, and load errors.", R"({"type":"object","properties":{}})", SolersToolExposure::DIRECT, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_list(args); }, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(access);
				return accesses; });
	_add("addon.ensure", "Install and enable one inspected exact Godot addon version. Completes after the editor filesystem scan has registered the addon's classes; success means files exist, extensions are loaded, editor plugins are enabled, and all Contract entry classes are registered.", R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1},"version":{"type":"string","minLength":1},"sha256":{"type":"string","pattern":"^[0-9a-fA-F]{64}$"}},"required":["source","plugin_id","version","sha256"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_INSTALL_PLUGIN, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure(args); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "project:res://addons/" + String(args.get("plugin_id", String())).to_lower();
				accesses.push_back(access);
				Dictionary lock;
				lock["mode"] = "write";
				lock["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(lock);
				return accesses; }, false, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure_finalize(args); }, [service](const SolersToolContext &, const Dictionary &args) { return service->addon_ensure_ready(args); }, {}, [](const Dictionary &args) { return SolersAssetService::is_trusted_addon(args) ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_INSTALL_PLUGIN; });
}

void SolersToolRegistry::_register_skill_tools() {
	_add_observe_exposed("skill.read", "Read one built-in Solers skill by exact name. Skills teach how to use existing native tools; they do not execute work.", R"({"type":"object","properties":{"name":{"type":"string","description":"Built-in skill name from the system skill catalog."}},"required":["name"]})", SolersToolExposure::DIRECT, [this](const SolersToolContext &, const Dictionary &a) {
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
				return _ok(data); }, {}, false, {}, {}, SolersToolUiKind::READ);
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *ref = reflection_service;
	const SolersPermissionManager::Permission edit_scene = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	_add_observe_exposed("object.query", "Query the live edited scene, a Resource path, an ObjectID, or native spatial facts between node pairs. node_paths scopes include_tree to those live subtrees; properties reads only the named native properties. Use target=resource for a PackedScene on disk.", R"({"type":"object","properties":{"target":{"type":"string","enum":["scene","resource","object","relations"]},"include_tree":{"type":"boolean"},"include_selection":{"type":"boolean"},"max_depth":{"type":"integer","minimum":0,"maximum":16},"max_children":{"type":"integer","minimum":1,"maximum":256},"node_paths":{"type":"array","items":{"type":"string"},"uniqueItems":true,"minItems":1,"maxItems":64},"include_connections":{"type":"boolean"},"path":{"type":"string","pattern":"^res://"},"type_hint":{"type":"string"},"include_dependencies":{"type":"boolean"},"max_dependencies":{"type":"integer","minimum":0,"maximum":2048},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128},"object_id":{},"relations":{"type":"array","minItems":1,"maxItems":128,"items":{"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}},"required":["a","b"],"additionalProperties":false}}},"required":["target"],"additionalProperties":false})", SolersToolExposure::DIRECT, [this, ref](const SolersToolContext &ctx, const Dictionary &a) {
				const String target = a.get("target", String());
				if (target == "relations") {
					Dictionary args = a.duplicate(true);
					args.erase("target");
					return ref->measure_spatial_relations(args);
				}
				if (target == "resource") {
					if (!resource_service) {
						return _error("RESOURCE_QUERY_UNAVAILABLE", "Resource queries are unavailable.", false);
					}
					Dictionary args = a.duplicate(true);
					args.erase("target");
					return resource_service->inspect_resource(args);
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
					return _ok(data);
				}

				Dictionary data;
				Node *edited_root = SceneTree::get_singleton()->get_edited_scene_root();
				if (!edited_root) {
					return _error("NO_EDITED_SCENE", "Open a scene before querying target=scene.", true);
				}
				const Array node_paths = a.get("node_paths", Array());
				if (observation_service && (bool)a.get("include_tree", false)) {
					const Dictionary scene_tree = observation_service->get_scene_tree(node_paths, (int)a.get("max_depth", 8), (int)a.get("max_children", 128), ctx.result_token_budget);
					data["scene_tree"] = scene_tree;
					if (!node_paths.is_empty() && Array(scene_tree.get("roots", Array())).is_empty()) {
						Dictionary result = _error("NODE_QUERY_FAILED", "None of the requested subtree roots exist in the live edited scene.");
						result["data"] = data;
						return result;
					}
				}
				if (observation_service && (bool)a.get("include_selection", false)) {
					data["selection"] = observation_service->get_selection(1, (int)a.get("max_children", 128));
				}
				if (!node_paths.is_empty() && (!(bool)a.get("include_tree", false) || a.has("properties") || (bool)a.get("include_connections", false))) {
					const Dictionary inspected = ref->inspect_nodes(a);
					if (!(bool)inspected.get("ok", false)) {
						return inspected;
					}
					data["details"] = inspected.get("data", Dictionary());
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
				return accesses; }, false, {}, {}, SolersToolUiKind::SCENE);
	_add("object.transaction", "Apply one scene UndoRedo transaction or one checkpointed Resource transaction. For either scope, update accepts one properties object copied from object.query facts.", R"({"type":"object","properties":{"scope":{"type":"string","enum":["scene","resource"]},"save_path":{"type":"string","pattern":"^res://","description":"Required only when creating an unsaved scene root."},"expected_state":{"type":"object","properties":{"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0},"root_object_id":{"type":"integer"},"scene_path":{"type":"string"}},"required":["history_id","version"],"additionalProperties":true},"operations":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"object","properties":{"op":{"type":"string","enum":["create_node","instantiate","reparent","connect_signal","attach_script","remove_node","bake_csg","create","update"]},"class_name":{"type":"string"},"name":{"type":"string"},"parent_path":{"type":"string"},"resource_path":{"type":"string","pattern":"^res://"},"properties":{"type":"object"},"node_path":{"type":"string"},"artifact":{"type":"string","enum":["mesh","collision"]},"hide_source":{"type":"boolean"},"new_parent_path":{"type":"string"},"position":{"type":"integer"},"source_path":{"type":"string"},"signal":{"type":"string"},"target_path":{"type":"string"},"method":{"type":"string"},"flags":{"type":"integer"},"script_path":{"type":"string","pattern":"^res://"},"path":{"type":"string","pattern":"^res://"},"type_hint":{"type":"string"},"expected_state":{"type":"object","properties":{"path":{"type":"string","pattern":"^res://"},"sha256":{"type":"string","pattern":"^[0-9a-fA-F]{64}$"},"uid":{"type":"string"}},"required":["path","sha256"],"additionalProperties":true}},"required":["op"],"additionalProperties":false}}},"required":["scope","operations"],"additionalProperties":false})", edit_scene, SolersToolMutationPolicy::EDITOR_UNDO, Vector<String>(), SolersToolExposure::DIRECT, [this](const SolersToolContext &, const Dictionary &a) { return _transact_objects(a); }, SolersToolExecution::MAIN_THREAD, [ref](const Dictionary &a) {
				const String scope = a.get("scope", String());
				if (scope == "scene") {
					return ref->resolve_batch_resource_access(a);
				}
				Array accesses;
				if (scope == "resource") {
					const Array operations = a.get("operations", Array());
					for (int i = 0; i < operations.size(); i++) {
						Dictionary access;
						access["mode"] = "write";
						access["key"] = "project:" + String(Dictionary(operations[i]).get("path", String()));
						accesses.push_back(access);
					}
				}
				return accesses; }, false, {}, {}, {}, [](const Dictionary &a) {
				const String scope = a.get("scope", String());
				return scope == "resource" ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_EDIT_SCENE; }, [](const Dictionary &a) {
				const String scope = a.get("scope", String());
				if (scope == "resource") {
					return SolersToolMutationPolicy::FILE_CHECKPOINT;
				}
				return SolersToolMutationPolicy::EDITOR_UNDO; });
	_add("scene.open", "Open a res:// scene via EditorInterface and return its native history receipt.",
			R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://","description":"Scene file to open."},"set_inherited":{"type":"boolean"}},"required":["path"],"additionalProperties":false})",
			SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->open_scene(a); });
	_add("mesh.unwrap_uv2", "Prepare UV2 for exact MeshInstance3D paths through Godot's native mesh API. ArrayMesh work runs one mesh at a time off the editor thread, reports progress, and commits one atomic UndoRedo action only after every surface verifies ARRAY_FORMAT_TEX_UV2. Imported static models should normally arrive with UV2 from Godot's Static Lightmaps importer mode.", R"({"type":"object","properties":{"node_paths":{"type":"array","minItems":1,"items":{"type":"string"}}},"required":["node_paths"]})", edit_scene, SolersToolMutationPolicy::EDITOR_UNDO, Vector<String>(), SolersToolExposure::DIRECT, [ref](const SolersToolContext &ctx, const Dictionary &a) { return ref->unwrap_uv2(a, ctx.call_id); }, SolersToolExecution::MAIN_THREAD, {}, false, [ref](const SolersToolContext &, const Dictionary &a) { return ref->poll_uv2_unwrap(a); }, [ref](const SolersToolContext &, const Dictionary &a) { return ref->is_uv2_unwrap_ready(a); }, [ref](const SolersToolContext &ctx, const Dictionary &, const Dictionary &result) {
				if (!(bool)result.get("ok", false)) {
					ref->cancel_uv2_unwrap(ctx.call_id);
				} });
	_add("lightmap.bake", "Bake one exact LightmapGI through Godot's native API. Before starting, reports every eligible or excluded MeshInstance3D in the LightmapGI parent subtree; an empty native bake scope fails immediately without opening the baker.",
			R"({"type":"object","properties":{"node_path":{"type":"string"},"data_path":{"type":"string","description":"Optional res://*.lmbake path; defaults from the saved scene."}},"required":["node_path"]})",
			edit_scene, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_lightmap(a); });

	_add_observe_exposed("engine.describe", "Search ClassDB or inspect exact classes. member_query returns matching typed members and documentation.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Fuzzy class search."},"inherits":{"type":"string"},"max_results":{"type":"integer","minimum":1,"maximum":200},"classes":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"class_name":{"type":"string","minLength":1},"include_inherited":{"type":"boolean"},"member_query":{"type":"string","description":"Filter typed members/docs; omit for names only."},"cursor":{"type":"integer","minimum":0,"description":"Cursor returned by the previous page. Default 0."},"max_members":{"type":"integer","minimum":1,"maximum":256,"description":"Required page size shared by methods, properties, signals, and constants."}},"required":["class_name","max_members"],"additionalProperties":false},"description":"Exact classes to introspect. Lean without member_query; expand with member_query."}},"additionalProperties":false})", SolersToolExposure::DIRECT, [this](const SolersToolContext &, const Dictionary &a) { return _inspect_engine(a); }, {}, true, {}, {}, SolersToolUiKind::SEARCH);
}

void SolersToolRegistry::_register_search_tools() {
	if (tools.has(SNAME("tool.search"))) {
		return;
	}
	bool has_deferred = false;
	for (const KeyValue<StringName, SolersTool *> &E : tools) {
		if (E.value->exposure() == SolersToolExposure::DEFERRED) {
			has_deferred = true;
			break;
		}
	}
	if (!has_deferred) {
		return;
	}
	_add_observe_exposed("tool.search", "Search third-party plugin, Connector, or MCP tools. Built-in Solers capabilities are always directly available.", R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Exact tool id, namespace, or capability terms."},"max_results":{"type":"integer","minimum":1,"maximum":50,"description":"Maximum tools to return. Default 10."}},"required":["query"],"additionalProperties":false})", SolersToolExposure::DIRECT, [this](const SolersToolContext &, const Dictionary &a) {
				const String query = String(a.get("query", String())).strip_edges();
				const int max_results = CLAMP((int)a.get("max_results", 10), 1, 50);
				Array matches;
				PackedStringArray corpora;
				Vector<StringName> names;
				HashSet<StringName> selected;
				for (const KeyValue<StringName, SolersTool *> &E : tools) {
					if (E.value->exposure() != SolersToolExposure::DEFERRED) {
						continue;
					}
					if (String(E.key) == query) {
						matches.push_back(_tool_to_dictionary(E.value));
						selected.insert(E.key);
						continue;
					}
					names.push_back(E.key);
					corpora.push_back(String(E.key) + " " + _tool_search_corpus(E.value));
				}
				if (matches.size() < max_results) {
					FuzzySearch search;
					search.max_results = max_results - matches.size();
					search.set_query(query, false);
					Vector<FuzzySearchResult> results;
					search.search_all(corpora, results);
					for (const FuzzySearchResult &result : results) {
						if (result.original_index < 0 || result.original_index >= names.size() || selected.has(names[result.original_index])) {
							continue;
						}
						SolersTool *const *tool = tools.getptr(names[result.original_index]);
						if (tool && *tool) {
							matches.push_back(_tool_to_dictionary(*tool));
						}
					}
				}
				// The result states which tools it opens up, so exposure is
				// granted by a field any tool may carry instead of by this
				// tool's name being recognized downstream.
				PackedStringArray unlocked;
				for (int i = 0; i < matches.size(); i++) {
					unlocked.push_back(Dictionary(matches[i]).get("name", String()));
				}
				Dictionary data;
				data["tools"] = matches;
				data["count"] = matches.size();
				data["unlock_tools"] = unlocked;
				return _ok(data); }, {}, false, {}, {}, SolersToolUiKind::SEARCH);
}

void SolersToolRegistry::register_tool(SolersTool *p_tool) {
	ERR_FAIL_NULL(p_tool);
	const bool deferred = p_tool->exposure() == SolersToolExposure::DEFERRED;
	_register(p_tool);
	if (deferred) {
		_register_search_tools();
	}
	_rebuild_tool_catalog();
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_add("history.revert", "Revert the latest reversible Agent mutation when its native UndoRedo version or file hashes still match.", R"({"type":"object","properties":{"reversal_id":{"type":"string","minLength":1}},"required":["reversal_id"],"additionalProperties":false})", SolersPermissionManager::PERMISSION_EDIT_SCENE, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT, [this](const SolersToolContext &ctx, const Dictionary &a) { return _revert_latest(ctx, a); }, SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
			Array accesses;
			Dictionary access;
			access["mode"] = "write";
			access["key"] = "*";
			accesses.push_back(access);
			return accesses; }, false, {}, {}, {}, [this](const Dictionary &a) {
			const Dictionary *record = reversals.getptr(String(a.get("reversal_id", String())));
			return record && String(record->get("policy", String())) == "file_checkpoint" ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_EDIT_SCENE; });
	_register_skill_tools();
	_register_reflection_tools();
	_register_observation_tools();
	_register_script_tools();
	_register_runtime_tools();
	_register_asset_tools();
	_register_addon_tools();
	_register_search_tools();
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
	tool["mutation_policy"] = _mutation_policy_name(cap.mutation_policy);
	tool["mutation_policy_dynamic"] = (bool)cap.mutation_policy_resolver;
	tool["execution"] = cap.execution == SolersToolExecution::WORKER_THREAD ? "worker" : "main_thread";
	tool["exposure"] = _exposure_name(p_tool->exposure());
	tool["ui_kind"] = _ui_kind_name(cap.ui_kind);
	tool["input_schema"] = p_tool->parameters_schema().duplicate(true);
	Dictionary object_schema;
	object_schema["type"] = "object";
	object_schema["properties"] = Dictionary();
	tool["output_schema"] = object_schema;
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

String SolersToolRegistry::get_skill_catalog_prompt() const {
	return SolersBuiltinSkills::build_catalog_prompt();
}

String SolersToolRegistry::get_model_tool_name(const StringName &p_name) const {
	if (!tools.has(p_name)) {
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
	if (tools.has(model_name)) {
		return model_name;
	}
	return StringName();
}

bool SolersToolRegistry::caches_across_revisions(const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	return tool && *tool && (*tool)->capability().cache_across_revisions;
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
		access["mode"] = cap.mutation_policy == SolersToolMutationPolicy::READ_ONLY ? "read" : "write";
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
	return _trace_args(normalize_tool_args(p_name, p_args), &(*tool_ptr)->capability());
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
	const SolersToolMutationPolicy resolved_policy = preflight_cap.mutation_policy_resolver ? preflight_cap.mutation_policy_resolver(args) : preflight_cap.mutation_policy;
	if (resolved_policy == SolersToolMutationPolicy::READ_ONLY) {
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
	r_call.mutation_policy = cap.mutation_policy_resolver ? cap.mutation_policy_resolver(args) : cap.mutation_policy;
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
	r_call.execution = cap.execution;
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

void SolersToolRegistry::restore_session_reversal(const String &p_session_id, const Dictionary &p_record) {
	if (p_session_id.is_empty()) {
		return;
	}
	const String *existing_id = latest_reversal_by_session.getptr(p_session_id);
	if (existing_id) {
		reversals.erase(*existing_id);
		latest_reversal_by_session.erase(p_session_id);
	}
	const String reversal_id = p_record.get("id", String());
	if (reversal_id.is_empty() || String(p_record.get("session_id", String())) != p_session_id) {
		return;
	}
	reversals[reversal_id] = p_record.duplicate(true);
	latest_reversal_by_session[p_session_id] = reversal_id;
}

int SolersToolRegistry::get_tool_count() const {
	return tools.size();
}

SolersToolRegistry::SolersToolRegistry() {}

SolersToolRegistry::~SolersToolRegistry() {
	_clear_tools();
	reversals.clear();
	latest_reversal_by_session.clear();
	delivered_addon_contracts.clear();
}
