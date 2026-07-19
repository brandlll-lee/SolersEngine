/**************************************************************************/
/*  solers_tool_registry.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                              SOLERS ENGINE                              */
/*                        (a fork of Godot Engine)                        */
/**************************************************************************/
/* Solers: AI-native game engine.                                        */
/**************************************************************************/

#include "solers_tool_registry.h"

#include "core/io/json.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/fuzzy_search.h"
#include "core/templates/hash_set.h"
#include "core/version.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/run/editor_run_bar.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_secret_store.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_trace.h"
#include "scene/main/node.h"

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
		if (String(schema.get("type", String())) == "object" && !schema.has("additionalProperties")) {
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
	delivered_plugin_contracts.clear();
}

void SolersToolRegistry::_register(SolersTool *p_tool) {
	const StringName name = p_tool->name();
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
		bool p_cache_across_revisions, SolersFunctionTool::PollHandler p_poll_handler, bool p_produces_scene_validation, SolersFunctionTool::ReadyHandler p_ready_handler, SolersFunctionTool::CompletionHandler p_completion_handler,
		std::function<SolersPermissionManager::Permission(const Dictionary &)> p_permission_resolver,
		std::function<SolersToolMutationPolicy(const Dictionary &)> p_mutation_policy_resolver) {
	SolersToolCapability cap;
	cap.permission = p_permission;
	cap.permission_resolver = std::move(p_permission_resolver);
	cap.mutation_policy = p_mutation_policy;
	cap.mutation_policy_resolver = std::move(p_mutation_policy_resolver);
	cap.cache_across_revisions = p_cache_across_revisions;
	cap.produces_scene_validation = p_produces_scene_validation;
	cap.execution = p_execution;
	cap.resource_access = std::move(p_resource_access);
	cap.redact_args = p_redact;
	SolersTool *tool = memnew(SolersFunctionTool(StringName(String::utf8(p_name)), String::utf8(p_description),
			_schema(p_schema_json), p_exposure, cap, std::move(p_handler), std::move(p_poll_handler), std::move(p_ready_handler), std::move(p_completion_handler)));
	_register(tool);
}

void SolersToolRegistry::_add_observe_exposed(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler,
		std::function<Array(const Dictionary &)> p_resource_access, bool p_cache_across_revisions, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler) {
	_add(p_name, p_description, p_schema_json, SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationPolicy::READ_ONLY,
			Vector<String>(), p_exposure, std::move(p_handler),
			SolersToolExecution::MAIN_THREAD, std::move(p_resource_access), p_cache_across_revisions, std::move(p_poll_handler), false, std::move(p_ready_handler));
}

void SolersToolRegistry::_add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler) {
	_add_observe_exposed(p_name, p_description, p_schema_json, SolersToolExposure::DIRECT, std::move(p_handler), std::move(p_resource_access), false, std::move(p_poll_handler), std::move(p_ready_handler));
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
	if (r_call.mutation_policy == SolersToolMutationPolicy::READ_ONLY || r_call.mutation_policy == SolersToolMutationPolicy::IRREVERSIBLE) {
		return Dictionary();
	}

	Dictionary state;
	state["policy"] = _mutation_policy_name(r_call.mutation_policy);
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO) {
		int history_id = EditorUndoRedoManager::INVALID_HISTORY;
		UndoRedo *undo_redo = _current_scene_undo_redo(history_id);
		if (!undo_redo) {
			return _tool_result_envelope(_error("UNDO_HISTORY_UNAVAILABLE", "The current edited scene has no UndoRedo history.", true), r_call.context.call_id);
		}
		state["history_id"] = history_id;
		state["version_before"] = (int64_t)undo_redo->get_version();
		r_call.reversal_state = state;
		return Dictionary();
	}

	if (!file_checkpoint) {
		return _tool_result_envelope(_error("CHECKPOINT_SERVICE_UNAVAILABLE", "The file checkpoint service is not initialized.", false), r_call.context.call_id);
	}
	Array checkpoints;
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
			return _tool_result_envelope(checkpoint, r_call.context.call_id);
		}
		checkpoints.push_back(checkpoint.get("data", Dictionary()));
	}
	if (checkpoints.is_empty()) {
		return _tool_result_envelope(_error("CHECKPOINT_TARGET_UNDECLARED", vformat("Tool '%s' must declare concrete project file write targets.", r_call.name), false), r_call.context.call_id);
	}
	state["checkpoints"] = checkpoints;
	r_call.reversal_state = state;
	return Dictionary();
}

void SolersToolRegistry::_persist_reversal_event(const SolersToolContext &p_context, const String &p_event, const Dictionary &p_record) const {
	if (p_context.project_path.is_empty() || p_context.session_id.is_empty()) {
		return;
	}
	Dictionary event;
	event["event_type"] = p_event;
	event["project_path"] = p_context.project_path;
	event["session_id"] = p_context.session_id;
	event["authored_revision"] = (int64_t)(p_context.authored_revision + 1);
	if (!p_record.is_empty()) {
		event["reversal"] = p_record;
	}
	solers_transcript_write(event);
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
				if (!file_checkpoint || !(bool)file_checkpoint->restore_checkpoint_state(checkpoints[i]).get("ok", false)) {
					return false;
				}
			}
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
	if (!affects_authored_state(r_call.name)) {
		return result;
	}

	Dictionary data = result.get("data", Dictionary());
	bool changed = (bool)data.get("authored_state_changed", true);
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
		data["authored_state_changed"] = false;
		result["data"] = data;
		return result;
	}

	Dictionary mutation;
	mutation["authored_revision"] = (int64_t)(r_call.context.authored_revision + 1);
	mutation["policy"] = _mutation_policy_name(r_call.mutation_policy);
	const String session_key = r_call.context.session_id.is_empty() ? String("direct") : r_call.context.session_id;
	if (r_call.mutation_policy == SolersToolMutationPolicy::EDITOR_UNDO || r_call.mutation_policy == SolersToolMutationPolicy::FILE_CHECKPOINT) {
		const String *previous_id = latest_reversal_by_session.getptr(session_key);
		if (previous_id) {
			reversals.erase(*previous_id);
		}
		const String reversal_id = (session_key + ":" + r_call.context.call_id + ":" + String::num_uint64(r_call.context.authored_revision + 1) + ":" + String::num_int64(reversals.size() + 1)).sha256_text();
		record["id"] = reversal_id;
		record["session_id"] = session_key;
		record["authored_revision"] = (int64_t)(r_call.context.authored_revision + 1);
		reversals[reversal_id] = record;
		latest_reversal_by_session[session_key] = reversal_id;
		mutation["reversal_id"] = reversal_id;
		_persist_reversal_event(r_call.context, "reversal_created", record);
	} else {
		const String *previous_id = latest_reversal_by_session.getptr(session_key);
		if (previous_id) {
			reversals.erase(*previous_id);
			latest_reversal_by_session.erase(session_key);
			_persist_reversal_event(r_call.context, "reversal_cleared");
		}
	}
	data["mutation"] = mutation;
	result["data"] = data;
	return result;
}

Dictionary SolersToolRegistry::_revert_latest(const SolersToolContext &p_context, const Dictionary &p_args) {
	const String reversal_id = String(p_args.get("reversal_id", String())).strip_edges();
	const uint64_t expected_revision = (int64_t)p_args.get("expected_revision", -1);
	if (expected_revision != p_context.authored_revision) {
		return _error("REVISION_CONFLICT", vformat("Expected authored revision %d, current revision is %d.", expected_revision, p_context.authored_revision));
	}
	const Dictionary *record_ptr = reversals.getptr(reversal_id);
	if (!record_ptr) {
		return _error("REVERSAL_NOT_FOUND", "The reversal id is unknown or has already been used.");
	}
	const Dictionary record = *record_ptr;
	const String session_key = p_context.session_id.is_empty() ? String("direct") : p_context.session_id;
	const String *latest = latest_reversal_by_session.getptr(session_key);
	if (!latest || *latest != reversal_id || String(record.get("session_id", String())) != session_key || (uint64_t)(int64_t)record.get("authored_revision", -1) != expected_revision) {
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

	reversals.erase(reversal_id);
	latest_reversal_by_session.erase(session_key);
	_persist_reversal_event(p_context, "reversal_consumed");
	Dictionary data;
	data["reversal_id"] = reversal_id;
	data["reverted_revision"] = (int64_t)expected_revision;
	data["authored_state_changed"] = true;
	return _ok(data);
}

Dictionary SolersToolRegistry::_compact_plugin_contract(const SolersToolContext &p_context, const Dictionary &p_result) {
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
	if (delivered_plugin_contracts.has(delivery_key)) {
		Dictionary compact;
		compact["contract_id"] = contract_id;
		compact["unchanged"] = true;
		data["agent_contract"] = compact;
		result["data"] = data;
		return result;
	}
	delivered_plugin_contracts.insert(delivery_key);
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

Dictionary SolersToolRegistry::_execute_engine(const Dictionary &p_args) {
	if (!resource_service || !reflection_service) {
		return _error("ENGINE_EXECUTION_UNAVAILABLE", "Native engine execution is unavailable.", false);
	}

	Dictionary refs;
	Array results;
	const Array operations = p_args.get("operations", Array());
	for (int i = 0; i < operations.size(); i++) {
		const Dictionary operation = operations[i];
		const String op = operation.get("op", String());
		const String id = operation.get("id", String());
		if (!id.is_empty() && refs.has(id)) {
			return _error("ENGINE_RESULT_ID_DUPLICATE", vformat("Operation result id '%s' is already defined.", id));
		}

		Variant object_id = operation.get("object_id", Variant());
		const String ref = operation.get("ref", String());
		if (!ref.is_empty()) {
			if (!refs.has(ref)) {
				return _error("ENGINE_RESULT_REF_UNKNOWN", vformat("Operation %d references unknown prior result '%s'.", i, ref));
			}
			object_id = refs[ref];
		}

		Dictionary result;
		if (op == "instantiate" || op == "load") {
			Dictionary args;
			if (op == "instantiate") {
				args["class_name"] = operation.get("class_name", String());
				result = resource_service->native_instantiate(args);
			} else {
				args["path"] = operation.get("path", String());
				args["type_hint"] = operation.get("type_hint", String());
				result = resource_service->native_load(args);
			}
		} else if (op == "editor_call") {
			Dictionary args;
			args["method"] = operation.get("method", String());
			args["args"] = operation.get("args", Array());
			result = reflection_service->invoke_editor(args);
		} else {
			if (object_id.get_type() == Variant::NIL) {
				return _error("ENGINE_OBJECT_REQUIRED", vformat("Operation %d requires object_id or ref.", i));
			}
			Dictionary args;
			args["object_id"] = object_id;
			if (op == "get" || op == "set") {
				args["property"] = operation.get("property", String());
				if (op == "set") {
					args["value"] = operation.get("value", Variant());
					result = resource_service->native_set(args);
				} else {
					result = resource_service->native_get(args);
				}
			} else if (op == "call") {
				args["method"] = operation.get("method", String());
				args["args"] = operation.get("args", Array());
				result = resource_service->native_call(args);
			} else if (op == "save") {
				args["path"] = operation.get("path", String());
				result = resource_service->native_save(args);
			} else if (op == "free") {
				result = resource_service->native_free(args);
			} else {
				return _error("ENGINE_OPERATION_UNKNOWN", vformat("Unknown engine.execute operation '%s'.", op));
			}
		}

		Dictionary step;
		step["index"] = i;
		step["op"] = op;
		step["result"] = result;
		results.push_back(step);
		if (!(bool)result.get("ok", false)) {
			Dictionary failure = _error("ENGINE_EXECUTION_FAILED", vformat("engine.execute stopped at operation %d (%s).", i, op));
			Dictionary data;
			data["results"] = results;
			data["failed_index"] = i;
			data["cause"] = result.get("error", Dictionary());
			failure["data"] = data;
			return failure;
		}

		if (!id.is_empty()) {
			const Dictionary data = result.get("data", Dictionary());
			Variant handle;
			if (data.has("object_id")) {
				handle = data;
			} else if (data.get("result", Variant()).get_type() == Variant::DICTIONARY && Dictionary(data.get("result", Dictionary())).has("object_id")) {
				handle = data.get("result", Dictionary());
			} else if (data.get("value", Variant()).get_type() == Variant::DICTIONARY && Dictionary(data.get("value", Dictionary())).has("object_id")) {
				handle = data.get("value", Dictionary());
			} else if (data.get("object", Variant()).get_type() == Variant::DICTIONARY) {
				handle = data.get("object", Dictionary());
			}
			if (handle.get_type() == Variant::NIL) {
				return _error("ENGINE_RESULT_NOT_REFERENCEABLE", vformat("Operation %d did not return an object handle for id '%s'.", i, id));
			}
			refs[id] = handle;
		}
	}

	Dictionary data;
	data["results"] = results;
	data["count"] = results.size();
	return _ok(data);
}

Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	ERR_FAIL_NULL_V(run_bar, _error("EDITOR_RUN_BAR_UNAVAILABLE", "The editor runtime controller is not available.", false));

	const String action = p_args.get("action", String());
	const bool was_playing = run_bar->is_playing();
	if (action == "play_current_scene") {
		if (!was_playing && !editor_interface->get_edited_scene_root()) {
			return _error("CURRENT_SCENE_UNAVAILABLE", "Open a scene before starting the project.");
		}
		if (!was_playing) {
			run_bar->play_current_scene();
		}
	} else if (action == "stop") {
		if (was_playing) {
			run_bar->stop_playing();
		}
	} else {
		return _error("INVALID_ARGUMENT", "action must be play_current_scene or stop.");
	}

	Dictionary poll_args;
	poll_args["action"] = action;
	poll_args["started_by_call"] = action == "play_current_scene" && !was_playing;
	poll_args["stopped_by_call"] = action == "stop" && was_playing;
	poll_args["already_playing"] = action == "play_current_scene" && was_playing;
	poll_args["deadline_msec"] = (int64_t)(OS::get_singleton()->get_ticks_msec() + 10000);
	if (_is_runtime_control_ready(poll_args)) {
		return _poll_runtime_control(poll_args);
	}
	Dictionary data = poll_args.duplicate(true);
	data["status"] = "pending";
	data["poll_args"] = poll_args;
	return _ok(data);
}

bool SolersToolRegistry::_is_runtime_control_ready(const Dictionary &p_args) const {
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	if (!run_bar) {
		return true;
	}
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const bool debugger_connected = debugger && debugger->is_session_active();
	const bool playing = run_bar->is_playing();
	const String action = p_args.get("action", String());
	const bool confirmed = action == "play_current_scene" ? playing && debugger_connected : !playing && !debugger_connected;
	return confirmed || OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0);
}

Dictionary SolersToolRegistry::_poll_runtime_control(const Dictionary &p_args) const {
	EditorRunBar *run_bar = EditorRunBar::get_singleton();
	ERR_FAIL_NULL_V(run_bar, _error("EDITOR_RUN_BAR_UNAVAILABLE", "The editor runtime controller is not available.", false));
	EditorDebuggerNode *debugger_node = EditorDebuggerNode::get_singleton();
	ScriptEditorDebugger *debugger = debugger_node ? debugger_node->get_current_debugger() : nullptr;
	const bool debugger_connected = debugger && debugger->is_session_active();
	const bool playing = run_bar->is_playing();
	const String action = p_args.get("action", String());
	const bool confirmed = action == "play_current_scene" ? playing && debugger_connected : !playing && !debugger_connected;
	if (!confirmed) {
		if (OS::get_singleton()->get_ticks_msec() >= (uint64_t)(int64_t)p_args.get("deadline_msec", 0)) {
			return _error("RUNTIME_STATE_TIMEOUT", vformat("EngineDebugger did not confirm runtime action '%s' within 10 seconds.", action));
		}
		Dictionary data = p_args.duplicate(true);
		data["status"] = "pending";
		data["poll_args"] = p_args;
		return _ok(data);
	}
	Dictionary data;
	data["action"] = action;
	data["is_playing"] = playing;
	data["debugger_connected"] = debugger_connected;
	data["playing_scene"] = run_bar->get_playing_scene();
	data["started_by_call"] = p_args.get("started_by_call", false);
	data["stopped_by_call"] = p_args.get("stopped_by_call", false);
	data["already_playing"] = p_args.get("already_playing", false);
	return _ok(data);
}

void SolersToolRegistry::_register_observation_tools() {
	if (!observation_service) {
		return;
	}
	SolersObservationService *obs = observation_service;

	_add_observe_exposed("project.search", "Search project paths, text, script symbols, scene nodes, or resource dependencies using Godot's indexed project facts.",
			R"({"oneOf":[{"type":"object","properties":{"type":{"type":"string","enum":["path","text","scene","symbol","dependency"],"description":"Fact domain to search."},"query":{"type":"string","minLength":1,"description":"Case-insensitive search text."},"max_results":{"type":"integer","minimum":1,"maximum":256,"description":"Maximum results. Default 64."}},"required":["type","query"],"additionalProperties":false},{"type":"object","properties":{"type":{"const":"dependency"},"path":{"type":"string","minLength":1,"description":"Return direct dependencies of this res:// resource."},"max_results":{"type":"integer","minimum":1,"maximum":256,"description":"Maximum results. Default 64."}},"required":["type","path"],"additionalProperties":false}]})",
			SolersToolExposure::DIRECT,
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->search_project(a)); });
	_add_observe("project.read_file", "Read a project file from res:// with project-root boundary and byte limits.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to read."},"max_bytes":{"type":"integer","description":"Maximum bytes to return. Default 262144."}},"required":["path"]})",
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->read_project_file(a.get("path", String()), (int)a.get("max_bytes", 262144))); },
			_access_by_arg("read", "project:", "path"));
	_add_observe("runtime.observe", "Read bounded EngineDebugger events since a cursor: lifecycle, output, errors, breaks, profiler data, and remote-scene changes.",
			R"({"type":"object","properties":{"since_cursor":{"type":"integer","minimum":0,"description":"Return events after this cursor. Default 0."},"types":{"type":"array","items":{"type":"string","enum":["started","stopped","output","error","break","debug_data","performance","remote_scene"]},"uniqueItems":true,"description":"Optional event type filter."},"max_events":{"type":"integer","minimum":1,"maximum":256,"description":"Maximum events. Default 128."}}})",
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->observe_runtime(a)); }, {},
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->observe_runtime(a)); },
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->is_runtime_observation_ready(a); });
	_add_observe_exposed("viewport.capture", "Capture explicit visual evidence. orthographic and top_down use a transient Camera3D/SubViewport sharing the live World3D; focus_paths frames exact scene subtrees and section_position places an explicit world-space cut plane without hiding or mutating nodes. Captures are frame-gated and content-addressed.",
			R"({"type":"object","properties":{"target":{"type":"string","enum":["editor","camera","top_down","orthographic","runtime"],"description":"Viewport source to capture."},"node_path":{"type":"string","description":"For target=camera: path of the Camera3D to render through. Defaults to the first camera in the scene."},"axis":{"type":"string","enum":["x","y","z"],"description":"For target=orthographic: world view axis."},"direction":{"type":"string","enum":["positive","negative"],"description":"For target=orthographic: side of the world axis from which the camera looks."},"focus_paths":{"type":"array","items":{"type":"string"},"description":"Optional edited-scene subtrees whose visible geometry defines framing."},"section_position":{"type":"number","description":"Optional world-space camera cut position on the view axis, useful for looking inside enclosed rooms."}},"required":["target"]})",
			SolersToolExposure::DIRECT,
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->capture_viewport(a); }, {}, false,
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->poll_viewport_capture(a); },
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->is_viewport_capture_ready(a); });

	if (resource_service) {
		SolersResourceService *svc = resource_service;
		_add_observe_exposed("resource.inspect", "Inspect one Godot Resource, its dependencies, native geometry facts, and selected property values.",
				R"({"type":"object","properties":{"path":{"type":"string","pattern":"^res://","description":"Godot resource path."},"type_hint":{"type":"string"},"include_dependencies":{"type":"boolean"},"max_dependencies":{"type":"integer","minimum":0,"maximum":2048},"properties":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":128}},"required":["path"],"additionalProperties":false})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->inspect_resource(a); },
				_access_by_arg("read", "project:", "path"));
		_add("resource.edit", "Create or update one Resource atomically, save it, and verify it can be reloaded. Resource-typed properties accept a res:// path string.",
				R"({"oneOf":[{"type":"object","properties":{"action":{"const":"create"},"class_name":{"type":"string","minLength":1},"path":{"type":"string","pattern":"^res://"},"properties":{"type":"object"},"type_hint":{"type":"string"}},"required":["action","class_name","path"],"additionalProperties":false},{"type":"object","properties":{"action":{"const":"update"},"path":{"type":"string","pattern":"^res://"},"properties":{"type":"object","minProperties":1},"type_hint":{"type":"string"}},"required":["action","path","properties"],"additionalProperties":false}]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::FILE_CHECKPOINT, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_resource(a); },
				SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
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
	_add("project.edit", "Edit project settings through ProjectSettings, write an ordinary project data file, or create an empty directory. Raw writes to project.godot, scripts, scenes, resources, and import-pipeline formats are rejected.",
			R"({"oneOf":[{"type":"object","properties":{"operation":{"const":"settings"},"values":{"type":"object"},"erase":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["operation"],"additionalProperties":false},{"type":"object","properties":{"operation":{"const":"write_file"},"path":{"type":"string","pattern":"^res://"},"content":{"type":"string"}},"required":["operation","path","content"],"additionalProperties":false},{"type":"object","properties":{"operation":{"const":"create_directory"},"path":{"type":"string","pattern":"^res://","description":"res:// directory to create (recursively). Succeeds idempotently when it already exists."}},"required":["operation","path"],"additionalProperties":false}]})",
			SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::FILE_CHECKPOINT, project_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_project(a); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = String(a.get("operation", String())) == "settings" ? "project:res://project.godot" : "project:" + String(a.get("path", String()));
				accesses.push_back(access);
				return accesses;
			}, false, {}, false, {}, {}, {},
			[](const Dictionary &a) { return String(a.get("operation", String())) == "settings" ? SolersToolMutationPolicy::EDITOR_UNDO : SolersToolMutationPolicy::FILE_CHECKPOINT; });

	Vector<String> script_redact;
	script_redact.push_back("content");
	script_redact.push_back("old_text");
	script_redact.push_back("new_text");
	_add("script.edit", "Create a script or replace one text block. old_text matches the current file content with whitespace/typography-tolerant fallbacks, so copy it from the latest read without re-deriving hashes. The write commits, is checkpointed (reversible via history.revert), and returns the parser's full diagnostics plus the patched region as it now exists on disk; fix reported errors with a follow-up edit.",
			R"({"oneOf":[{"type":"object","properties":{"operation":{"const":"create"},"path":{"type":"string","pattern":"^res://.*\\.(gd|cs|gdshader|gdshaderinc)$"},"content":{"type":"string"}},"required":["operation","path","content"],"additionalProperties":false},{"type":"object","properties":{"operation":{"const":"replace"},"path":{"type":"string","pattern":"^res://.*\\.(gd|cs|gdshader|gdshaderinc)$"},"old_text":{"type":"string","minLength":1},"new_text":{"type":"string"},"occurrence":{"type":"integer","minimum":1}},"required":["operation","path","old_text","new_text"],"additionalProperties":false}]})",
			SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::FILE_CHECKPOINT, script_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->edit_script(a); },
			SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
	_add_observe_exposed("script.validate", "Validate script source through Godot's registered ScriptLanguage implementation.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the script to validate."},"source":{"type":"string","description":"Optional source override; validates this text instead of the file content."}},"required":["path"]})",
			SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_script(a); });
	Vector<String> run_redact;
	run_redact.push_back("source");
	_add("script.run", "Run a one-shot @tool GDScript inside the editor process for algorithmic or bulk work the typed tools cannot express (procedural data, batch edits). The source must start with @tool, extend a RefCounted type (default base or EditorScript), and define func run() or func run(host). The optional host argument is a temporary Node already inside the editor scene tree: add nodes that need tree access to it. By default the host and everything under it is freed when the call finishes; pass persist_host_children=true to instead hand every node the script parented under host over to the edited scene root as one undoable action (this is how scripted content such as GDExtension terrain becomes durable). await is supported - the tool completes when run() actually finishes (30 s limit). Returns printed output, script errors, and run()'s return value.",
			R"({"type":"object","properties":{"source":{"type":"string","minLength":1,"description":"Complete GDScript source. Starts with @tool and defines func run() or func run(host)."},"persist_host_children":{"type":"boolean","description":"Hand the nodes parented under host over to the edited scene root (undoable) instead of freeing them. Requires an open edited scene."}},"required":["source"],"additionalProperties":false})",
			SolersPermissionManager::PERMISSION_EDIT_SCENE, SolersToolMutationPolicy::IRREVERSIBLE, run_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_script(a); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "engine-native:";
				accesses.push_back(access);
				return accesses;
			}, false,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_script_finalize(a); }, false,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_script_ready(a); });
	_add("history.revert", "Revert the latest reversible Agent mutation when its authored revision and native UndoRedo or file checkpoint state still match.",
			R"({"type":"object","properties":{"reversal_id":{"type":"string","minLength":1},"expected_revision":{"type":"integer","minimum":0}},"required":["reversal_id","expected_revision"]})",
			SolersPermissionManager::PERMISSION_EDIT_SCENE, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[this](const SolersToolContext &ctx, const Dictionary &a) { return _revert_latest(ctx, a); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "*";
				accesses.push_back(access);
				return accesses;
			}, false, {}, false, {}, {},
			[this](const Dictionary &a) {
				const Dictionary *record = reversals.getptr(String(a.get("reversal_id", String())));
				return record && String(record->get("policy", String())) == "file_checkpoint" ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_EDIT_SCENE;
			});
}

void SolersToolRegistry::_register_runtime_tools() {
	const SolersPermissionManager::Permission run_project = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	_add("runtime.control", "Start or stop editor playback and complete only after EngineDebugger confirms the authoritative runtime state. Repeating an action is idempotent.",
			R"({"type":"object","properties":{"action":{"type":"string","enum":["play_current_scene","stop"]}},"required":["action"]})",
			run_project, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) { return _run_control(a); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "runtime:";
				accesses.push_back(access);
				return accesses;
			}, false, [this](const SolersToolContext &, const Dictionary &a) { return _poll_runtime_control(a); }, false,
			[this](const SolersToolContext &, const Dictionary &a) { return _is_runtime_control_ready(a); });
}

void SolersToolRegistry::_register_asset_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *svc = asset_service;
	_add("asset.catalog.search", "Search cached lightweight metadata from one official CC0 catalog. ambientCG and Poly Haven both support materials, HDRIs, and 3D models. Terms use coverage matching instead of strict AND; results explain matched_terms and matched_fields. Search never resolves files or downloads previews. Inspect one selected result before acquire.",
			R"({"type":"object","properties":{"provider":{"type":"string","enum":["ambientcg","polyhaven"],"description":"Official catalog to search."},"query":{"type":"string","description":"Material, HDRI, or 3D model search terms. Multiple terms broaden candidate coverage and improve ranking; they do not all have to match."},"kind":{"type":"string","enum":["material","hdri","3d"],"description":"Asset kind supported by both catalogs."},"limit":{"type":"integer","description":"Maximum returned results; default 20, maximum 50."},"offset":{"type":"integer","description":"Zero-based result offset."},"refresh":{"type":"boolean","description":"Explicitly refresh the provider's cached lightweight directory."}},"required":["provider","query","kind"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_search(a, ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD,
			[](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-directory:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower();
				accesses.push_back(access);
				return accesses;
			});
	_add("asset.catalog.inspect", "Resolve one exact search result against the provider's current official detail/files metadata. Returns versioned variants, dependencies and available checksums. This is the only source of arguments accepted by asset.catalog.acquire.",
			R"({"type":"object","properties":{"provider":{"type":"string","enum":["ambientcg","polyhaven"]},"kind":{"type":"string","enum":["material","hdri","3d"]},"asset_id":{"type":"string","description":"Exact asset_id returned by asset.catalog.search."},"refresh":{"type":"boolean","description":"Explicitly refresh this asset's cached official detail metadata."}},"required":["provider","kind","asset_id"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_inspect(a, ctx.cancel_requested); }, SolersToolExecution::WORKER_THREAD,
			[](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-catalog-detail:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("kind", String())).to_lower() + ":" + String(a.get("asset_id", String())).to_lower();
				accesses.push_back(access);
				return accesses;
			});
	_add("asset.generate", "Queue one reusable local asset generation job and return its manifest immediately. Solers observes progress and resumes this Agent session when the job reaches a terminal state. kind=3d uses Meshy by default; music/sfx use ElevenLabs by default. For 3D props, call only when the user authorized generation, after project, local Library, and catalog reuse were considered and the architectural shell was visually verified. One prop per job; after terminal resume, import_to_project and place the asset — do not re-generate on missing feel. Keep 3D prompts faithful and concise: describe only the isolated object's identity, shape, material, and requested style. Omit input_attachments for Text-to-3D. Pass input_attachments only when the user explicitly supplied isolated-object reference images for this exact asset; never pass a room, scene, composition, or general mood reference. Put generation controls in provider_options, not in the prompt. For a game-scene prop, explicitly request should_remesh=true, topology, and an integer target_polycount; import validates the real result rather than trusting the request. Provider-native options pass through provider_options.",
			R"({"type":"object","properties":{"kind":{"type":"string","description":"3d, music, or sfx."},"prompt":{"type":"string","description":"Asset generation prompt. Required unless input_attachments supplies explicit isolated-object references."},"input_attachments":{"type":"array","items":{"type":"string"},"description":"Explicit isolated-object image attachment ids only. Omit for Text-to-3D. One id uses Image-to-3D; two to four use Multi-Image-to-3D. Never pass a room or scene reference."},"name":{"type":"string","description":"Optional local asset display name."},"profile":{"type":"string","description":"game_default, high_quality, or custom. game_default uses standard 3D generation; set provider_options.model_type=\"lowpoly\" only when the user explicitly asks for low-poly."},"provider":{"type":"string","description":"Optional provider id. Defaults by kind."},"provider_options":{"type":"object","description":"Provider-native generation parameters. Meshy Image-to-3D supports image/texturing/remesh controls here.","properties":{"model_type":{"type":"string","enum":["standard","lowpoly"],"description":"Meshy model type. Default is standard; use lowpoly only if user asks for low-poly."},"ai_model":{"type":"string","enum":["meshy-5","meshy-6","latest"],"description":"Meshy model id. Defaults to meshy-6."},"pose_mode":{"type":"string","enum":["a-pose","t-pose"],"description":"Humanoid pose control when supported by the provider."},"should_texture":{"type":"boolean","description":"Generate textures. Meshy default is true."},"enable_pbr":{"type":"boolean","description":"Generate PBR maps. Requires should_texture=true."},"hd_texture":{"type":"boolean","description":"Request higher quality textures when Meshy supports it."},"texture_prompt":{"type":"string","description":"Guide Meshy texturing with text. Do not use together with texture_image_url."},"texture_image_url":{"type":"string","description":"Guide Meshy texturing with an image URL or data URI. Do not use together with texture_prompt."},"should_remesh":{"type":"boolean","description":"Enable Meshy remesh phase."},"topology":{"type":"string","enum":["triangle","quad"],"description":"Mesh topology when remeshing."},"target_polycount":{"type":"integer","description":"Target polygon count when remeshing."},"image_enhancement":{"type":"boolean","description":"Ask Meshy to enhance the input image before generation."},"remove_lighting":{"type":"boolean","description":"Ask Meshy to reduce baked lighting in generated textures when supported."},"moderation":{"type":"boolean","description":"Ask Meshy to screen input content before generation."},"auto_size":{"type":"boolean","description":"Ask Meshy to estimate physical size when supported."},"origin_at":{"type":"string","enum":["bottom","center"],"description":"Generated model origin placement when supported."},"save_pre_remeshed_model":{"type":"boolean","description":"Ask Meshy to keep the pre-remesh output when supported."},"alpha_thumbnail":{"type":"boolean","description":"Ask Meshy for transparent thumbnail when supported."},"target_formats":{"type":"array","items":{"type":"string","enum":["glb","obj","fbx","stl","usdz","3mf"]},"description":"Output formats to generate. Solers imports GLB best."}}}},"required":["kind"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->generate_for_session(a, ctx.session_id); });
	_add("asset.catalog.acquire", "Queue one exact variant returned by asset.catalog.inspect. The task revalidates official metadata, downloads the selected package, verifies available checksums, and stores licensing and attribution under one stable job id.",
			R"({"type":"object","properties":{"provider":{"type":"string","enum":["ambientcg","polyhaven"],"description":"Provider returned by asset.catalog.inspect."},"kind":{"type":"string","enum":["material","hdri","3d"],"description":"Kind returned by asset.catalog.inspect."},"asset_id":{"type":"string","description":"Exact official provider asset id."},"variant":{"type":"string","description":"Exact variants[].id returned by asset.catalog.inspect; never guess a format."},"source_version":{"type":"string","description":"Optional pin. When present it must equal the source_version of the cached asset.catalog.inspect result; when omitted, the cached inspection's current source_version is used automatically."},"name":{"type":"string","description":"Optional local display name."}},"required":["provider","kind","asset_id","variant"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_acquire(a, ctx.session_id); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "asset-library-entry:" + String(a.get("provider", String())).to_lower() + ":" + String(a.get("asset_id", String())) + ":" + String(a.get("variant", String()));
				accesses.push_back(access);
				return accesses;
			});
	_add_observe_exposed("asset.capabilities", "List the operations currently available for a Solers Library asset, including option schemas, provider documentation links, and provider action catalogs such as Meshy animation_actions.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id."}},"required":["asset_id"]})",
			SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->capabilities(a); });
	_add("asset.run_operation", "Queue one available operation on an existing Solers Library asset. The operation creates a new derived asset, never overwrites the source, and Solers resumes this Agent session when it reaches a terminal state.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id."},"operation_id":{"type":"string","description":"Operation id returned by asset.capabilities."},"options":{"type":"object","description":"Operation options that match the schema returned by asset.capabilities."},"raw_provider_options":{"type":"object","description":"Advanced provider-native options. Requires raw_confirmed=true."},"raw_confirmed":{"type":"boolean","description":"Set true only after the user explicitly confirms raw provider options."}},"required":["asset_id","operation_id"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->run_operation_for_session(a, ctx.session_id); });
	_add_observe_exposed("asset.status", "Read a local asset generation task or generated asset manifest by asset_id.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id returned by asset.generate or asset.list_local."}},"required":["asset_id"]})",
			SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->status(a); },
			_access_by_arg("read", "asset:", "asset_id"));
	_add_observe_exposed("job.wait", "Declare the background asset job ids needed before the Agent can continue. This returns their current terminal or pending states immediately. Keep doing conflict-free work when possible; when nothing remains, call this once and stop issuing tools so the same Agent turn waits until any requested job reaches a terminal state.",
			R"({"type":"object","properties":{"ids":{"type":"array","items":{"type":"string"},"description":"Stable job ids returned by asset.generate, asset.catalog.acquire, or asset.run_operation."}},"required":["ids"]})",
			SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->wait_jobs(a, ctx.session_id); },
			[](const Dictionary &a) {
				Array accesses;
				const Array ids = a.get("ids", Array());
				for (int i = 0; i < ids.size(); i++) {
					Dictionary access;
					access["mode"] = "read";
					access["key"] = "asset:" + String(ids[i]);
					accesses.push_back(access);
				}
				return accesses;
			});
	_add_observe_exposed("asset.list_local", "List reusable assets in the local Solers Library. Optional kind/query filters search manifests only.",
			R"({"type":"object","properties":{"kind":{"type":"string","description":"Optional 3d, material, hdri, music, or sfx."},"query":{"type":"string","description":"Optional text search over name and prompt."},"limit":{"type":"integer","description":"Maximum assets to return. Default 128."}}})",
			SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->list_local(a); },
			[](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "asset-library:";
				accesses.push_back(access);
				return accesses;
			});
	_add("asset.import_to_project", "Copy a ready Solers Library asset into res://, then import it through the editor's frame-budgeted incremental pipeline (the editor stays responsive) and resume once every selected dependency actually loads. 3D imports enforce a source triangle budget from glTF metadata before any file is copied; oversized assets are rejected with TOPOLOGY_BUDGET_EXCEEDED so you can pick a smaller variant or remesh first. Declare import_profile=\"baked_static\" only when the scene will bake lightmaps — it configures Godot's native Static Lightmaps mode (UV2 unwrap), which is expensive on dense meshes. Texture-set materials require map_types chosen from the manifest's exact map roles, so unused alternatives are not imported.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id."},"target_dir":{"type":"string","description":"Optional res:// destination directory. Defaults to res://solers_assets/<kind>/<name>."},"import_profile":{"type":"string","enum":["runtime","baked_static"],"description":"3D import intent. \"runtime\" (default) imports geometry as-is; \"baked_static\" additionally configures Godot's Static Lightmaps mode with UV2 unwrap for lightmap baking."},"max_triangles":{"type":"integer","minimum":0,"description":"Source triangle budget for 3D imports. Defaults to the asset's remesh target or the project's solers/ai_assets/import/max_source_triangles setting. Declarations above that project setting (or 0 to disable it) are rejected while the setting is non-zero; raise the setting through project.edit settings first when the project can truly afford more density."},"map_types":{"type":"array","items":{"type":"string"},"description":"For texture-set materials, exact map role names from manifest.map_files that the final material will reference. Omit for native materials, HDRIs, and 3D assets."}},"required":["asset_id"]})",
			SolersPermissionManager::PERMISSION_EDIT_FILES, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->import_to_project(a); },
			SolersToolExecution::WORKER_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary asset;
				asset["mode"] = "read";
				asset["key"] = "asset:" + String(a.get("asset_id", String()));
				accesses.push_back(asset);
				String target_dir = String(a.get("target_dir", String())).strip_edges();
				if (target_dir.is_empty()) {
					target_dir = "res://solers_assets/";
				}
				Dictionary target;
				target["mode"] = "write";
				target["key"] = "project:" + target_dir.replace_char('\\', '/').simplify_path();
				accesses.push_back(target);
				return accesses;
			}, false,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->poll_project_import(a); }, false,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->is_project_import_ready(a); },
			[svc](const SolersToolContext &, const Dictionary &a, const Dictionary &r) { svc->release_project_import(a, r); });
}

void SolersToolRegistry::_register_plugin_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *service = asset_service;
	_add("plugin.search", "Search installable Godot editor plugins. Verified Solers bundles are ranked first; remaining results come from the official Godot Asset Library.",
			R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Plugin name or capability."},"limit":{"type":"integer","minimum":1,"maximum":50,"description":"Maximum results. Default 20."}},"required":["query"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT,
			[service](const SolersToolContext &ctx, const Dictionary &args) { return service->plugin_search(args, ctx.cancel_requested); },
			SolersToolExecution::WORKER_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "plugin-catalog:";
				accesses.push_back(access);
				return accesses;
			});
	_add("plugin.inspect", "Inspect one exact plugin before installation. Returns inert package facts plus an optional bounded, data-only Agent Contract; repeated identical contracts are returned by id without reinjecting their full content.",
			R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1,"description":"Exact plugin_id returned by plugin.search."},"refresh":{"type":"boolean","description":"Redownload Asset Library metadata and archive instead of reusing the inert cache."}},"required":["source","plugin_id"]})",
			SolersPermissionManager::PERMISSION_NETWORK, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT,
			[this, service](const SolersToolContext &ctx, const Dictionary &args) { return _compact_plugin_contract(ctx, service->plugin_inspect(args, ctx.cancel_requested)); },
			SolersToolExecution::WORKER_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "plugin-cache:" + String(args.get("source", String())) + ":" + String(args.get("plugin_id", String()));
				accesses.push_back(access);
				return accesses;
			}, false, {}, false, {}, {}, [](const Dictionary &args) {
				return SolersAssetService::is_trusted_plugin(args) ? SolersPermissionManager::PERMISSION_OBSERVE : SolersPermissionManager::PERMISSION_NETWORK;
			});
	_add_observe_exposed("plugin.list", "List plugins installed through Solers, including pinned version, source, package hash, enabled state, registered ClassDB types, missing files, restart requirements, and load errors.",
			R"({"type":"object","properties":{}})", SolersToolExposure::DIRECT,
			[service](const SolersToolContext &, const Dictionary &args) { return service->plugin_list(args); },
			[](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(access);
				return accesses;
			});
	_add("plugin.ensure", "Install and enable one inspected exact plugin version. Completes after the editor filesystem scan has registered the plugin's classes; success means files exist, extensions are loaded, editor plugins are enabled, and all Contract entry classes are registered.",
			R"({"type":"object","properties":{"source":{"type":"string","enum":["bundled","assetlib"]},"plugin_id":{"type":"string","minLength":1},"version":{"type":"string","minLength":1},"sha256":{"type":"string","pattern":"^[0-9a-fA-F]{64}$"}},"required":["source","plugin_id","version","sha256"],"additionalProperties":false})",
			SolersPermissionManager::PERMISSION_INSTALL_PLUGIN, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[service](const SolersToolContext &, const Dictionary &args) { return service->plugin_ensure(args); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &args) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "project:res://addons/" + String(args.get("plugin_id", String())).to_lower();
				accesses.push_back(access);
				Dictionary lock;
				lock["mode"] = "write";
				lock["key"] = "project:res://.solers/plugins.lock.json";
				accesses.push_back(lock);
				return accesses;
			}, false,
			[service](const SolersToolContext &, const Dictionary &args) { return service->plugin_ensure_finalize(args); }, false,
			[service](const SolersToolContext &, const Dictionary &args) { return service->plugin_ensure_ready(args); }, {},
			[](const Dictionary &args) {
				return SolersAssetService::is_trusted_plugin(args) ? SolersPermissionManager::PERMISSION_EDIT_FILES : SolersPermissionManager::PERMISSION_INSTALL_PLUGIN;
			});
}

void SolersToolRegistry::_register_skill_tools() {
	_add_observe_exposed("skill.read", "Read one built-in Solers skill by exact name. Skills teach how to use existing native tools; they do not execute work.",
			R"({"type":"object","properties":{"name":{"type":"string","description":"Built-in skill name from the system skill catalog."}},"required":["name"]})",
			SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) {
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
				return _ok(data);
			});
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *ref = reflection_service;
	const SolersPermissionManager::Permission edit_scene = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	_add_observe_exposed("scene.inspect", "Inspect the edited scene tree, current selection, and optional exact node properties or signal connections in one bounded read.",
			R"({"type":"object","properties":{"include_tree":{"type":"boolean"},"include_selection":{"type":"boolean"},"max_depth":{"type":"integer","minimum":0,"maximum":16},"max_children":{"type":"integer","minimum":1,"maximum":256},"node_paths":{"type":"array","items":{"type":"string"},"uniqueItems":true,"maxItems":64},"include_properties":{"type":"boolean"},"include_connections":{"type":"boolean"},"max_properties":{"type":"integer","minimum":1,"maximum":512}},"additionalProperties":false})",
			SolersToolExposure::DIRECT,
			[this, ref](const SolersToolContext &, const Dictionary &a) {
				Dictionary data;
				if (observation_service && (bool)a.get("include_tree", true)) {
					data["scene_tree"] = observation_service->get_scene_tree((int)a.get("max_depth", 8), (int)a.get("max_children", 128));
				}
				if (observation_service && (bool)a.get("include_selection", true)) {
					data["selection"] = observation_service->get_selection(1, (int)a.get("max_children", 128));
				}
				if (a.has("node_paths")) {
					const Dictionary inspected = ref->inspect_nodes(a);
					if (!(bool)inspected.get("ok", false)) {
						return inspected;
					}
					data["details"] = inspected.get("data", Dictionary());
				}
				return _ok(data);
			});
	_add("scene.edit", "Atomically create, instantiate, configure, reparent, connect, attach, or remove scene nodes. The whole operation list is one EditorUndoRedo action and rolls back at the first failure.",
			R"({"type":"object","properties":{"operations":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"object","properties":{"op":{"type":"string","enum":["create_node","instantiate","set_property","reparent","connect_signal","attach_script","remove_node"]},"class_name":{"type":"string"},"name":{"type":"string"},"parent_path":{"type":"string"},"resource_path":{"type":"string","pattern":"^res://"},"properties":{"type":"object"},"node_path":{"type":"string"},"property":{"type":"string"},"value":{},"new_parent_path":{"type":"string"},"position":{"type":"integer"},"source_path":{"type":"string"},"signal":{"type":"string"},"target_path":{"type":"string"},"method":{"type":"string"},"flags":{"type":"integer"},"script_path":{"type":"string","pattern":"^res://"}},"required":["op"],"additionalProperties":false}}},"required":["operations"],"additionalProperties":false})",
			edit_scene, SolersToolMutationPolicy::EDITOR_UNDO, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->batch(a); }, SolersToolExecution::MAIN_THREAD,
			[ref](const Dictionary &a) { return ref->resolve_batch_resource_access(a); });
	_add("scene.validate", "Measure either explicit spatial relations or complete structure/support/reference-layout constraints against the live edited scene.",
			R"({"type":"object","properties":{"mode":{"type":"string","enum":["spatial","structure"]},"relations":{"type":"array","items":{"type":"object"}},"structure_roots":{"type":"array","items":{"type":"string"}},"placement_roots":{"type":"array","items":{"type":"string"}},"placements":{"type":"array","items":{"type":"object"}},"reference_layout":{"type":"object"}},"required":["mode","relations"],"additionalProperties":false})",
			SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationPolicy::READ_ONLY, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) {
				Dictionary args = a.duplicate(true);
				const String mode = args.get("mode", String());
				args.erase("mode");
				return mode == "spatial" ? ref->validate_spatial_relations(args) : ref->validate_structure(args);
			}, SolersToolExecution::MAIN_THREAD, {}, false, {}, true);
	_add("scene.scatter", "Scatter up to 262144 seeded, area-weighted instances of a mesh resource across a MeshInstance3D surface as one MultiMeshInstance3D child. The transform buffer persists to its own res:// resource so the scene file stays small; re-running with the same seed reproduces the layout.",
			R"({"type":"object","properties":{"surface_path":{"type":"string","minLength":1,"description":"MeshInstance3D that receives the instances."},"mesh_path":{"type":"string","pattern":"^res://","description":"Mesh resource to scatter."},"count":{"type":"integer","minimum":1,"maximum":262144},"seed":{"type":"integer"},"name":{"type":"string","minLength":1},"scale_min":{"type":"number","exclusiveMinimum":0},"scale_max":{"type":"number","exclusiveMinimum":0},"align_to_normal":{"type":"boolean","description":"Align instance up axis to the surface normal. Default true."},"random_yaw":{"type":"boolean","description":"Randomize rotation around the up axis. Default true."},"max_slope_degrees":{"type":"number","minimum":0,"maximum":90,"description":"Reject samples on faces steeper than this. Default 90 (no filter)."},"multimesh_path":{"type":"string","pattern":"^res://","description":"Where to save the MultiMesh resource; defaults next to the scene."}},"required":["surface_path","mesh_path","count"],"additionalProperties":false})",
			edit_scene, SolersToolMutationPolicy::EDITOR_UNDO, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->scatter_instances(a); });
	_add("scene.bake_csg", "Bake exact CSG root node paths directly through Godot's CSG API into static MeshInstance3D artifacts. This operation does not depend on editor selection.",
			R"({"type":"object","properties":{"node_paths":{"type":"array","minItems":1,"items":{"type":"string"}},"hide_sources":{"type":"boolean","description":"Hide source CSG roots after a successful atomic bake. Default true."}},"required":["node_paths"]})",
			edit_scene, SolersToolMutationPolicy::EDITOR_UNDO, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_csg(a); });
	_add("mesh.unwrap_uv2", "Prepare UV2 for exact MeshInstance3D paths through Godot's native mesh API. ArrayMesh work runs one mesh at a time off the editor thread, reports progress, and commits one atomic UndoRedo action only after every surface verifies ARRAY_FORMAT_TEX_UV2. Imported static models should normally arrive with UV2 from Godot's Static Lightmaps importer mode.",
			R"({"type":"object","properties":{"node_paths":{"type":"array","minItems":1,"items":{"type":"string"}}},"required":["node_paths"]})",
			edit_scene, SolersToolMutationPolicy::EDITOR_UNDO, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &ctx, const Dictionary &a) { return ref->unwrap_uv2(a, ctx.call_id); },
			SolersToolExecution::MAIN_THREAD, {}, false,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->poll_uv2_unwrap(a); }, false,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->is_uv2_unwrap_ready(a); },
			[ref](const SolersToolContext &ctx, const Dictionary &, const Dictionary &result) {
				if (!(bool)result.get("ok", false)) {
					ref->cancel_uv2_unwrap(ctx.call_id);
				}
			});
	_add("lightmap.bake", "Bake one exact LightmapGI through Godot's native API. Before starting, reports every eligible or excluded MeshInstance3D in the LightmapGI parent subtree; an empty native bake scope fails immediately without opening the baker.",
			R"({"type":"object","properties":{"node_path":{"type":"string"},"data_path":{"type":"string","description":"Optional res://*.lmbake path; defaults from the saved scene."}},"required":["node_path"]})",
			edit_scene, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_lightmap(a); });

	_add_observe_exposed("engine.inspect", "Search Godot/plugin ClassDB by query, or introspect exact classes for full signatures and integrated documentation. Inspect a class before calling unfamiliar API through engine.execute.",
			R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Fuzzy class search."},"inherits":{"type":"string"},"max_results":{"type":"integer","minimum":1,"maximum":200},"classes":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"class_name":{"type":"string","minLength":1},"include_inherited":{"type":"boolean"},"member_query":{"type":"string"}},"required":["class_name"],"additionalProperties":false},"description":"Exact classes to introspect with signatures and docs."}},"additionalProperties":false})",
			SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) { return _inspect_engine(a); }, {}, true);
	_add("engine.execute", "Execute a bounded sequence of typed RefCounted/Resource or EditorInterface operations against the live engine. Failures report the exact operation index and native cause. Scene and ordinary Resource editing must use their domain tools.",
			R"({"type":"object","properties":{"operations":{"type":"array","minItems":1,"maxItems":128,"items":{"type":"object","properties":{"id":{"type":"string","minLength":1},"op":{"type":"string","enum":["instantiate","load","get","set","call","save","free","editor_call"]},"class_name":{"type":"string"},"path":{"type":"string","pattern":"^res://"},"type_hint":{"type":"string"},"object_id":{},"ref":{"type":"string"},"property":{"type":"string"},"value":{},"method":{"type":"string"},"args":{"type":"array"}},"required":["op"],"additionalProperties":false}}},"required":["operations"],"additionalProperties":false})",
			edit_scene, SolersToolMutationPolicy::IRREVERSIBLE, Vector<String>(), SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) { return _execute_engine(a); },
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "engine-native:";
				accesses.push_back(access);
				return accesses;
			});
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
	_add_observe_exposed("tool.search", "Search third-party plugin, Connector, or MCP tools. Built-in Solers capabilities are always directly available.",
			R"({"type":"object","properties":{"query":{"type":"string","minLength":1,"description":"Exact tool id, namespace, or capability terms."},"max_results":{"type":"integer","minimum":1,"maximum":50,"description":"Maximum tools to return. Default 10."}},"required":["query"],"additionalProperties":false})",
			SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) {
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
				Dictionary data;
				data["tools"] = matches;
				data["count"] = matches.size();
				return _ok(data);
			});
}

void SolersToolRegistry::register_tool(SolersTool *p_tool) {
	ERR_FAIL_NULL(p_tool);
	const bool deferred = p_tool->exposure() == SolersToolExposure::DEFERRED;
	_register(p_tool);
	if (deferred) {
		_register_search_tools();
	}
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_register_skill_tools();
	_register_reflection_tools();
	_register_observation_tools();
	_register_script_tools();
	_register_runtime_tools();
	_register_asset_tools();
	_register_plugin_tools();
	_register_search_tools();
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
	tool["produces_scene_validation"] = cap.produces_scene_validation;
	tool["execution"] = cap.execution == SolersToolExecution::WORKER_THREAD ? "worker" : "main_thread";
	tool["exposure"] = _exposure_name(p_tool->exposure());
	tool["input_schema"] = p_tool->parameters_schema().duplicate(true);
	Dictionary object_schema;
	object_schema["type"] = "object";
	object_schema["properties"] = Dictionary();
	tool["output_schema"] = object_schema;
	return tool;
}

Array SolersToolRegistry::list_tools() const {
	Array result;
	Vector<String> names;
	for (const KeyValue<StringName, SolersTool *> &E : tools) {
		names.push_back(String(E.key));
	}
	names.sort();
	for (int i = 0; i < names.size(); i++) {
		SolersTool *const *tool = tools.getptr(StringName(names[i]));
		if (tool && *tool) {
			result.push_back(_tool_to_dictionary(*tool));
		}
	}
	return result;
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

bool SolersToolRegistry::affects_authored_state(const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	if (!tool || !*tool) {
		return false;
	}
	const SolersPermissionManager::Permission permission = (*tool)->capability().permission;
	return permission == SolersPermissionManager::PERMISSION_EDIT_SCENE || permission == SolersPermissionManager::PERMISSION_EDIT_FILES || permission == SolersPermissionManager::PERMISSION_INSTALL_PLUGIN;
}

bool SolersToolRegistry::affects_scene_state(const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	return tool && *tool && (*tool)->capability().permission == SolersPermissionManager::PERMISSION_EDIT_SCENE;
}

bool SolersToolRegistry::produces_scene_validation(const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	return tool && *tool && (*tool)->capability().produces_scene_validation;
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

Dictionary SolersToolRegistry::protect_tool_args_for_replay(const StringName &p_name, const Dictionary &p_args) const {
	Dictionary out = p_args.duplicate(true);
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return out;
	}
	const SolersToolCapability &cap = (*tool_ptr)->capability();
	for (int i = 0; i < cap.redact_args.size(); i++) {
		const String &key = cap.redact_args[i];
		if (!out.has(key)) {
			continue;
		}
		const String payload = SolersSecretStore::protect(JSON::stringify(out[key], "", false, true));
		if (!SolersSecretStore::is_protected(payload)) {
			out[key] = "<redacted>";
			continue;
		}
		Dictionary protected_value;
		protected_value["protected"] = true;
		protected_value["encoding"] = "json";
		protected_value["payload"] = payload;
		out[key] = protected_value;
	}
	return out;
}

Dictionary SolersToolRegistry::restore_tool_args_from_replay(const StringName &p_name, const Dictionary &p_args) const {
	Dictionary out = p_args.duplicate(true);
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return out;
	}
	const SolersToolCapability &cap = (*tool_ptr)->capability();
	for (int i = 0; i < cap.redact_args.size(); i++) {
		const String &key = cap.redact_args[i];
		if (!out.has(key) || out[key].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary protected_value = out[key];
		if (!(bool)protected_value.get("protected", false) || String(protected_value.get("encoding", String())) != "json") {
			continue;
		}
		const String plain = SolersSecretStore::unprotect(protected_value.get("payload", String()));
		if (!plain.is_empty()) {
			out[key] = JSON::parse_string(plain);
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
	const Dictionary result = _execute_prepared_tool(call);
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
	const Dictionary result = _finalize_prepared_result(p_call, _tool_result_envelope(p_call.tool->execute(p_call.context, p_call.args), p_call.context.call_id));
	SOLERS_TRACE("registry.execute_end", vformat("%s %s", String(p_call.name), summarize_tool_result_for_audit(result)));
	return result;
}

Dictionary SolersToolRegistry::_poll_prepared_tool(SolersPreparedToolCall &p_call, const Dictionary &p_args) {
	ERR_FAIL_NULL_V(p_call.tool, _error("TOOL_NOT_FOUND", "Prepared Solers tool is unavailable.", false));
	SOLERS_TRACE("registry.poll_begin", vformat("%s args=%s", String(p_call.name), _trace_json(summarize_tool_args_for_audit(p_call.name, p_args), 420)));
	const Dictionary result = _finalize_prepared_result(p_call, _tool_result_envelope(p_call.tool->poll(p_call.context, p_args), p_call.context.call_id));
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
	for (const String &key : delivered_plugin_contracts) {
		if (key.begins_with(delivery_prefix)) {
			delivery_keys.push_back(key);
		}
	}
	for (const String &key : delivery_keys) {
		delivered_plugin_contracts.erase(key);
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
	delivered_plugin_contracts.clear();
}
