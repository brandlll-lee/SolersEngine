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

#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_asset_service.h"
#include "modules/solers_ai/core/solers_file_checkpoint.h"
#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_trace.h"

void SolersToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_project_observation", "project_observation"), &SolersToolRegistry::set_project_observation);
	ClassDB::bind_method(D_METHOD("set_runtime_observation", "runtime_observation"), &SolersToolRegistry::set_runtime_observation);
	ClassDB::bind_method(D_METHOD("set_scene_observation", "scene_observation"), &SolersToolRegistry::set_scene_observation);
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
		case SolersToolExposure::DEFERRED:
			return "deferred";
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

PackedStringArray SolersToolRegistry::_mutation_domain_names(SolersToolMutationDomain p_domains) {
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

struct SolersToolUiPresentation {
	const char *kind;
	const char *running;
	const char *completed;
	const char *failed;
};

static constexpr SolersToolUiPresentation SOLERS_TOOL_UI_PRESENTATIONS[] = {
	{ "default", "Running", "Completed", "Failed" },
	{ "observe", "Observing", "Observed", "Failed to observe" },
	{ "read", "Reading", "Read", "Failed to read" },
	{ "search", "Searching", "Searched", "Failed to search" },
	{ "write", "Writing", "Wrote", "Failed to write" },
	{ "scene", "Updating", "Updated", "Failed to update" },
	{ "shell", "Running", "Completed", "Failed to run" },
	{ "run", "Running", "Completed", "Failed to run" },
	{ "network", "Fetching", "Fetched", "Failed to fetch" },
	{ "asset", "Processing", "Processed", "Failed to process" },
	{ "capture", "Capturing", "Captured", "Failed to capture" },
	{ "think", "Thinking", "Thought", "Failed to think" },
	{ "shield", "Authorizing", "Authorized", "Failed to authorize" },
};

static const SolersToolUiPresentation &_ui_presentation(SolersToolUiKind p_kind) {
	const int index = (int)p_kind;
	const int count = sizeof(SOLERS_TOOL_UI_PRESENTATIONS) / sizeof(SOLERS_TOOL_UI_PRESENTATIONS[0]);
	return SOLERS_TOOL_UI_PRESENTATIONS[index >= 0 && index < count ? index : 0];
}

static const char *_ui_kind_name(SolersToolUiKind p_kind) {
	return _ui_presentation(p_kind).kind;
}

static SolersToolUiKind _resolved_ui_kind(const SolersToolCapability &p_capability) {
	if (p_capability.ui_kind != SolersToolUiKind::DEFAULT) {
		return p_capability.ui_kind;
	}
	if (p_capability.operation_mode == SolersOperationMode::QUERY) {
		return p_capability.permission == SolersPermissionManager::PERMISSION_NETWORK ? SolersToolUiKind::NETWORK : SolersToolUiKind::OBSERVE;
	}
	if (solers_has_mutation_domain(p_capability.mutation_domains, SolersToolMutationDomain::FILES)) {
		return SolersToolUiKind::WRITE;
	}
	if (solers_has_mutation_domain(p_capability.mutation_domains, SolersToolMutationDomain::EDITOR)) {
		return SolersToolUiKind::SCENE;
	}
	return p_capability.permission == SolersPermissionManager::PERMISSION_SHELL ? SolersToolUiKind::SHELL : SolersToolUiKind::RUN;
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

Dictionary SolersToolRegistry::_tool_result_envelope(const Dictionary &p_result, const String &p_call_id) {
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

std::function<Array(const Dictionary &)> SolersToolRegistry::_access_by_arg(const char *p_mode, const char *p_prefix, const char *p_arg) {
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
					PackedStringArray supported;
					for (const Variant &property : properties.keys()) {
						supported.push_back(String(property));
					}
					supported.sort();
					r_error = vformat("%s.%s is not supported. Supported fields: %s.", p_path, *key, supported.is_empty() ? String("none") : String(", ").join(supported));
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

static Variant _ui_subject_at_pointer(const Dictionary &p_args, const String &p_pointer) {
	if (!p_pointer.begins_with("/")) {
		return Variant();
	}
	Variant value = p_args;
	const PackedStringArray segments = p_pointer.split("/", true);
	for (int i = 1; i < segments.size(); i++) {
		const String segment = segments[i].replace("~1", "/").replace("~0", "~");
		if (value.get_type() == Variant::DICTIONARY) {
			value = Dictionary(value).get(segment, Variant());
		} else if (value.get_type() == Variant::ARRAY && segment.is_valid_int()) {
			const Array values = value;
			const int index = segment.to_int();
			value = index >= 0 && index < values.size() ? values[index] : Variant();
		} else {
			return Variant();
		}
	}
	return value;
}

static String _ui_subject_text(const Variant &p_value) {
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		return String(p_value).strip_edges();
	}
	if (p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT || p_value.get_type() == Variant::BOOL) {
		return p_value.stringify();
	}
	return String();
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
	if (p_tool->exposure() == SolersToolExposure::DEFERRED && capability.operation_domain == SolersOperationDomain::NONE) {
		ERR_PRINT(vformat("Deferred Solers tool '%s' must declare a capability domain.", name));
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
	if (p_tool->exposure() != SolersToolExposure::HIDDEN) {
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
		std::function<bool(const Dictionary &)> p_execution_ready, const PackedStringArray &p_ui_subject_paths) {
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
	cap.ui_subject_paths = p_ui_subject_paths;
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
		SolersOperationDomain p_operation_domain, SolersOperationMode p_operation_mode, const PackedStringArray &p_ui_subject_paths) {
	_add(p_name, p_description, p_schema_json, SolersPermissionManager::PERMISSION_OBSERVE, SolersToolMutationDomain::NONE,
			Vector<String>(), p_exposure, std::move(p_handler),
			p_execution, std::move(p_resource_access), std::move(p_poll_handler), std::move(p_ready_handler), {}, {}, {}, p_ui_kind, p_host, StringName(), p_operation_domain, p_operation_mode, {}, StringName(), {}, p_ui_subject_paths);
}

void SolersToolRegistry::_add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler,
		SolersToolUiKind p_ui_kind, const PackedStringArray &p_ui_subject_paths) {
	_add_observe_exposed(p_name, p_description, p_schema_json, SolersToolExposure::MODEL, std::move(p_handler), std::move(p_resource_access), std::move(p_poll_handler), std::move(p_ready_handler), p_ui_kind, SolersToolExecution::MAIN_THREAD, {}, SolersOperationDomain::NONE, SolersOperationMode::QUERY, p_ui_subject_paths);
}

void SolersToolRegistry::_add_operation(SolersOperationDomain p_domain, SolersOperationMode p_mode,
		const char *p_name, const char *p_description, const char *p_schema_json,
		SolersPermissionManager::Permission p_permission, SolersToolMutationDomain p_mutation_domains,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access,
		const StringName &p_target_class,
		SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler, SolersFunctionTool::CompletionHandler p_completion_handler,
		const StringName &p_target_kind) {
	const SolersToolUiKind ui_kind = p_mode == SolersOperationMode::QUERY ? SolersToolUiKind::OBSERVE : (p_domain == SolersOperationDomain::EDITOR ? SolersToolUiKind::SCENE : SolersToolUiKind::RUN);
	_add(p_name, p_description, p_schema_json, p_permission, p_mutation_domains, {}, SolersToolExposure::DEFERRED,
			std::move(p_handler), SolersToolExecution::MAIN_THREAD, std::move(p_resource_access), std::move(p_poll_handler), std::move(p_ready_handler), std::move(p_completion_handler), {}, {}, ui_kind, {}, p_target_class, p_domain, p_mode, {}, p_target_kind);
}

void SolersToolRegistry::set_project_observation(SolersProjectObservation *p_project_observation) {
	project_observation = p_project_observation;
}

void SolersToolRegistry::set_runtime_observation(SolersRuntimeObservation *p_runtime_observation) {
	runtime_observation = p_runtime_observation;
}

void SolersToolRegistry::set_scene_observation(SolersSceneObservation *p_scene_observation) {
	scene_observation = p_scene_observation;
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

Dictionary SolersToolRegistry::_model_input_schema(const SolersTool *p_tool) const {
	Dictionary schema = p_tool->parameters_schema().duplicate(true);
	const SolersToolCapability &capability = p_tool->capability();
	const bool accepts_state = capability.mutation_domain_resolver ||
			solers_has_mutation_domain(capability.mutation_domains, SolersToolMutationDomain::EDITOR) ||
			solers_has_mutation_domain(capability.mutation_domains, SolersToolMutationDomain::FILES);
	if (!accepts_state) {
		return schema;
	}
	Dictionary properties = schema.get("properties", Dictionary());
	properties["expected_state"] = _schema(R"({"type":"object","description":"Optional native precondition copied from scene state and resource state receipts. Include every file the operation will write.","properties":{"has_root":{"type":"boolean"},"root_object_id":{"type":"string","pattern":"^-?[0-9]+$"},"scene_path":{"type":"string"},"history_id":{"type":"integer"},"version":{"type":"integer","minimum":0},"resource_uid":{"type":"string"},"saved_sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"},"resources":{"type":"array","uniqueItems":true,"items":{"type":"object","properties":{"path":{"type":"string","pattern":"^res://"},"exists":{"type":"boolean"},"sha256":{"type":"string","pattern":"^[0-9a-f]{64}$"},"directory":{"type":"boolean"},"resource_uid":{"type":"string"}},"required":["path","exists"],"additionalProperties":false}}},"additionalProperties":false})");
	schema["properties"] = properties;
	return schema;
}

void SolersToolRegistry::register_tool(SolersTool *p_tool) {
	ERR_FAIL_NULL(p_tool);
	_register(p_tool);
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
	const SolersToolUiKind ui_kind = _resolved_ui_kind(cap);
	const SolersToolUiPresentation &presentation = _ui_presentation(ui_kind);
	tool["ui_kind"] = _ui_kind_name(ui_kind);
	tool["ui"] = Dictionary({ { "running", presentation.running }, { "completed", presentation.completed }, { "failed", presentation.failed } });
	tool["ui_subject_paths"] = cap.ui_subject_paths;
	tool["timeline_visible"] = cap.host.timeline_visible;
	tool["attachment_args"] = cap.host.attachment_args;
	tool["required_model_inputs"] = cap.host.required_model_inputs;
	tool["input_schema"] = _model_input_schema(p_tool);
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
	if (!tool || !*tool || (*tool)->exposure() == SolersToolExposure::HIDDEN) {
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
	if (tool && *tool && (*tool)->exposure() != SolersToolExposure::HIDDEN) {
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
	return _trace_args(normalized, &capability);
}

String SolersToolRegistry::summarize_tool_args_for_ui(const StringName &p_name, const Dictionary &p_args) const {
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return String();
	}
	const Dictionary args = normalize_tool_args(p_name, p_args);
	for (const String &path : (*tool_ptr)->capability().ui_subject_paths) {
		const String subject = _ui_subject_text(_ui_subject_at_pointer(args, path));
		if (!subject.is_empty()) {
			return subject;
		}
	}
	for (const Variant &value : resolve_resource_access(p_name, args)) {
		const String key = Dictionary(value).get("key", String());
		const int separator = key.find(":");
		if (key != "*" && separator >= 0 && separator + 1 < key.length()) {
			return key.substr(separator + 1);
		}
	}
	return String();
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
	const Dictionary input_schema = _model_input_schema(tool);
	if (resolved_domains == SolersToolMutationDomain::NONE) {
		_solers_clamp_numeric_args_to_schema(args, input_schema);
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
	if (!_validate_tool_schema_value(schema_args, input_schema, "parameters", argument_error)) {
		Dictionary invalid = _error("TOOL_ARGUMENT_INVALID", argument_error, true);
		invalid["data"] = Dictionary({ { "arguments", redact_tool_args_for_audit(p_name, args) } });
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
	const Dictionary state_error = _validate_expected_state(tool, args);
	if (!state_error.is_empty()) {
		return _tool_result_envelope(state_error, p_context.call_id);
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
	handler_args.erase("expected_state");
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

int SolersToolRegistry::get_tool_count() const {
	return tools.size();
}

SolersToolRegistry::SolersToolRegistry() {}

SolersToolRegistry::~SolersToolRegistry() {
	_clear_tools();
	reversals_by_session.clear();
	delivered_addon_contracts.clear();
}
