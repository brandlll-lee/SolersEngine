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
#include "core/object/class_db.h"
#include "editor/editor_interface.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_reflection_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_trace.h"

void SolersToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_observation_service", "observation_service"), &SolersToolRegistry::set_observation_service);
	ClassDB::bind_method(D_METHOD("set_reflection_service", "reflection_service"), &SolersToolRegistry::set_reflection_service);
	ClassDB::bind_method(D_METHOD("set_resource_service", "resource_service"), &SolersToolRegistry::set_resource_service);
	ClassDB::bind_method(D_METHOD("set_script_service", "script_service"), &SolersToolRegistry::set_script_service);
	ClassDB::bind_method(D_METHOD("set_permission_manager", "permission_manager"), &SolersToolRegistry::set_permission_manager);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersToolRegistry::set_action_timeline);
	ClassDB::bind_method(D_METHOD("register_default_tools"), &SolersToolRegistry::register_default_tools);
	ClassDB::bind_method(D_METHOD("list_tools"), &SolersToolRegistry::list_tools);
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
		case SolersToolExposure::DIRECT_MODEL_ONLY:
			return "direct_model_only";
		case SolersToolExposure::HIDDEN:
			return "hidden";
	}
	return "direct";
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
		return parsed;
	}
	ERR_PRINT("Solers tool registered with an invalid params schema; falling back to a generic object.");
	return generic;
}

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

static void _add_unique_search_token(Vector<String> &r_tokens, const String &p_token) {
	const String token = p_token.strip_edges();
	if (token.is_empty()) {
		return;
	}
	for (int i = 0; i < r_tokens.size(); i++) {
		if (r_tokens[i] == token) {
			return;
		}
	}
	r_tokens.push_back(token);
}

static Vector<String> _search_tokens(const String &p_text) {
	Vector<String> tokens;
	const String text = p_text.to_lower();
	String token;
	for (int i = 0; i < text.length(); i++) {
		const char32_t c = text[i];
		const bool is_token_char = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (is_token_char) {
			token += String::chr(c);
		} else {
			_add_unique_search_token(tokens, token);
			token.clear();
		}
	}
	_add_unique_search_token(tokens, token);
	return tokens;
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

static bool _tool_matches_tokens(const SolersTool *p_tool, const Vector<String> &p_tokens) {
	if (p_tokens.is_empty()) {
		return false;
	}
	const String corpus = _tool_search_corpus(p_tool);
	for (int i = 0; i < p_tokens.size(); i++) {
		if (!corpus.contains(p_tokens[i])) {
			return false;
		}
	}
	return true;
}

void SolersToolRegistry::_clear_tools() {
	for (KeyValue<StringName, SolersTool *> &E : tools) {
		memdelete(E.value);
	}
	tools.clear();
	model_name_index.clear();
}

void SolersToolRegistry::_register(SolersTool *p_tool) {
	const StringName name = p_tool->name();
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
		SolersPermissionManager::Permission p_permission, const char *p_mutation_kind,
		bool p_requires_approval, bool p_undoable, const Vector<String> &p_redact,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler) {
	SolersToolCapability cap;
	cap.permission = p_permission;
	cap.mutation_kind = p_mutation_kind ? String::utf8(p_mutation_kind) : String("none");
	cap.requires_approval = p_requires_approval;
	cap.undoable = p_undoable;
	cap.redact_args = p_redact;
	SolersTool *tool = memnew(SolersFunctionTool(StringName(String::utf8(p_name)), String::utf8(p_description),
			_schema(p_schema_json), p_exposure, cap, std::move(p_handler)));
	_register(tool);
}

void SolersToolRegistry::_add_observe_exposed(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler) {
	_add(p_name, p_description, p_schema_json, SolersPermissionManager::PERMISSION_OBSERVE, "none",
			/*requires_approval*/ false, /*undoable*/ false, Vector<String>(), p_exposure, std::move(p_handler));
}

void SolersToolRegistry::_add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersFunctionTool::Handler p_handler) {
	_add_observe_exposed(p_name, p_description, p_schema_json, SolersToolExposure::DIRECT, std::move(p_handler));
}

void SolersToolRegistry::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
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

Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	const String action = p_args.get("action", String());
	if (action == "play_current_scene") {
		editor_interface->play_current_scene();
	} else if (action == "stop") {
		editor_interface->stop_playing_scene();
	} else {
		return _error("INVALID_ARGUMENT", "action must be play_current_scene or stop.");
	}

	Dictionary data;
	data["action"] = action;
	data["is_playing"] = editor_interface->is_playing_scene();
	data["playing_scene"] = editor_interface->get_playing_scene();
	return _ok(data);
}

void SolersToolRegistry::_register_observation_tools() {
	if (!observation_service) {
		return;
	}
	SolersObservationService *obs = observation_service;

	_add_observe_exposed("project.search_files", "Search project file paths under res:// by case-insensitive substring; omit query to list a bounded file index.",
			R"({"type":"object","properties":{"query":{"type":"string","description":"Optional case-insensitive substring matched against res:// paths. Empty lists bounded project files."},"max_files":{"type":"integer","description":"Maximum number of matches to return (1-2000). Default 128."}}})",
			SolersToolExposure::DEFERRED,
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->search_project_files(a.get("query", String()), (int)a.get("max_files", 128))); });
	_add_observe("project.read_file", "Read a project file from res:// with project-root boundary and byte limits.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to read."},"max_bytes":{"type":"integer","description":"Maximum bytes to return. Default 262144."}},"required":["path"]})",
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->read_project_file(a.get("path", String()), (int)a.get("max_bytes", 262144))); });
	_add_observe("editor.get_snapshot", "Read a combined project, scene, selection, and runtime snapshot.",
			R"({"type":"object","properties":{"max_scene_depth":{"type":"integer","description":"Maximum scene-tree depth to serialize. Default 4."},"max_children_per_node":{"type":"integer","description":"Maximum children serialized per node. Default 64."},"include_remote_scene":{"type":"boolean","description":"When true and the game is running, include the debugger remote scene tree. Default false."}}})",
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->get_editor_snapshot((int)a.get("max_scene_depth", 4), (int)a.get("max_children_per_node", 64), (bool)a.get("include_remote_scene", false))); });

	if (resource_service) {
		SolersResourceService *svc = resource_service;
		_add_observe_exposed("resource.get_info", "Read resource type, UID, import state, and dependency metadata for a res:// resource.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the resource."},"include_dependencies":{"type":"boolean","description":"Include dependency list. Default true."},"max_dependencies":{"type":"integer","description":"Maximum dependencies to return (0-2048). Default 128."}},"required":["path"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->get_resource_info(a); });
		_add("resource.create", "Instantiate any Godot Resource subclass through ClassDB and save it with ResourceSaver. class_name chooses the exact engine type.",
				R"({"type":"object","properties":{"class_name":{"type":"string","description":"Instantiable Resource class from class.introspect."},"path":{"type":"string","description":"res:// path to save."}},"required":["class_name","path"]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, "resource_save", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->create_resource(a); });
		_add_observe_exposed("resource.get_property", "Load a res:// Resource with ResourceLoader and read one native Object property.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// resource path."},"property":{"type":"string","description":"Native property name."},"type_hint":{"type":"string","description":"Optional ResourceLoader type hint."}},"required":["path","property"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->get_resource_property(a); });
		_add("resource.set_property", "Load a res:// Resource with ResourceLoader, set one native Object property, and save it with ResourceSaver.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// resource path."},"property":{"type":"string","description":"Native property name."},"value":{"description":"New value. Math types accept arrays; Color accepts {r,g,b,a}; Object properties accept res:// resource paths."},"type_hint":{"type":"string","description":"Optional ResourceLoader type hint."}},"required":["path","property","value"]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, "resource_save", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->set_resource_property(a); });
		_add("resource.call_method", "Load a res:// Resource with ResourceLoader and call one native Object method. Set save=true to write the mutated resource with ResourceSaver.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// resource path."},"method":{"type":"string","description":"Native method name."},"args":{"type":"array","description":"Positional arguments. Default []."},"save":{"type":"boolean","description":"Save after the call. Default false."},"type_hint":{"type":"string","description":"Optional ResourceLoader type hint."}},"required":["path","method"]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, "resource_save", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->call_resource_method(a); });
		_add_observe_exposed("export.list_presets", "List Godot export platforms and export presets from the current project.",
				R"({"type":"object","properties":{"include_platforms":{"type":"boolean","description":"Include available export platforms. Default true."}}})",
				SolersToolExposure::DEFERRED,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->list_export_presets(a); });
		_add_observe_exposed("export.validate_presets", "Validate configured export presets without exporting build artifacts.",
				R"({"type":"object","properties":{"debug":{"type":"boolean","description":"Validate against the debug export template. Default false."}}})",
				SolersToolExposure::DEFERRED,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_export_presets(a); });
		_add("export.run_preset", "Run Godot's native EditorExportPlatform::export_project for one export preset.",
				R"({"type":"object","properties":{"preset_index":{"type":"integer","description":"Export preset index from export.list_presets."},"preset_name":{"type":"string","description":"Export preset name when index is unknown."},"debug":{"type":"boolean","description":"Export debug build. Default false."},"export_path":{"type":"string","description":"Optional output path override; defaults to the preset export_path."}}})",
				SolersPermissionManager::PERMISSION_EXPORT_BUILD, "export_build", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->run_export_preset(a); });
	}
}

void SolersToolRegistry::_register_script_tools() {
	if (!script_service) {
		return;
	}
	SolersScriptService *svc = script_service;
	const SolersPermissionManager::Permission edit_files = SolersPermissionManager::PERMISSION_EDIT_FILES;
	const char *file_write_schema =
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to write."},"content":{"type":"string","description":"Full new text content. Mutually exclusive with content_base64."},"content_base64":{"type":"string","description":"Base64 raw bytes for binary assets. Mutually exclusive with content."},"create":{"type":"boolean","description":"Create the file when missing. Default true."},"overwrite":{"type":"boolean","description":"Overwrite existing content. Default true."},"validate_if_script":{"type":"boolean","description":"Run script-language validation for text content when the file is a script. Default true."}},"required":["path"]})";

	Vector<String> file_write_redact;
	file_write_redact.push_back("content");
	file_write_redact.push_back("content_base64");
	_add("project.write_file", "Write a project text or binary file with path safety, file checkpointing, optional script validation, and EditorFileSystem refresh.", file_write_schema,
			edit_files, "file_write", true, false, file_write_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->write_file(a); });
	Vector<String> file_patch_redact;
	file_patch_redact.push_back("old_text");
	file_patch_redact.push_back("new_text");
	_add("script.patch", "Apply an exact text replacement to a script or text file with optional sha256 guard, checkpointing, and validation.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to patch."},"old_text":{"type":"string","description":"Exact existing text to replace."},"new_text":{"type":"string","description":"Replacement text."},"occurrence":{"type":"integer","description":"1-based occurrence of old_text to replace. Default 1."},"expected_sha256":{"type":"string","description":"Optional sha256 the current file content must match."},"validate_if_script":{"type":"boolean","description":"Run script validation after patching. Default true."}},"required":["path","old_text","new_text"]})",
			edit_files, "file_patch", true, false, file_patch_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->patch_file(a); });
	_add_observe_exposed("script.validate", "Validate script source through Godot's registered ScriptLanguage implementation.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the script to validate."},"source":{"type":"string","description":"Optional source override; validates this text instead of the file content."}},"required":["path"]})",
			SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_script(a); });
}

void SolersToolRegistry::_register_runtime_tools() {
	const SolersPermissionManager::Permission run_project = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	_add("runtime.control", "Run primitive for controlling editor playback. action=play_current_scene starts the edited scene; action=stop stops playback.",
			R"({"type":"object","properties":{"action":{"type":"string","description":"play_current_scene or stop."}},"required":["action"]})",
			run_project, "runtime_only", true, false, Vector<String>(), SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) { return _run_control(a); });
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *ref = reflection_service;
	const SolersPermissionManager::Permission edit_scene = SolersPermissionManager::PERMISSION_EDIT_SCENE;

	_add("class.introspect", "Introspect any engine class via ClassDB: methods (with arg types), properties, signals, enum constants, and inheritance. Use this to learn the real Godot API on demand instead of guessing.",
			R"({"type":"object","properties":{"class_name":{"type":"string","description":"Engine class to introspect, e.g. Node3D, Sprite2D, CharacterBody2D."},"include_inherited":{"type":"boolean","description":"Include members inherited from parent classes. Default true."}},"required":["class_name"]})",
			SolersPermissionManager::PERMISSION_OBSERVE, "none", false, false, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->introspect_class(a); });
	_add("object.get_property", "Read one ClassDB-validated property off an in-tree node in the edited scene.",
			R"({"type":"object","properties":{"node_path":{"type":"string","description":"Path relative to the edited scene root. Default '.'."},"property":{"type":"string","description":"Property name; see class.introspect for valid names."}},"required":["property"]})",
			SolersPermissionManager::PERMISSION_OBSERVE, "none", false, false, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->get_property(a); });
	_add("object.set_property", "Write one ClassDB-validated, type-coerced property on an in-tree node through EditorUndoRedoManager (undoable). Math types accept component arrays; Object/Resource properties accept res:// resource paths.",
			R"({"type":"object","properties":{"node_path":{"type":"string","description":"Path relative to the edited scene root. Default '.'."},"property":{"type":"string","description":"Property name; see class.introspect."},"value":{"description":"New value. Pass numbers/strings/bools directly; pass math types as component arrays; pass res:// paths for Object/Resource properties."}},"required":["property","value"]})",
			edit_scene, "editor_undo_redo", true, true, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->set_property(a); });
	_add("object.call_method", "Call a ClassDB-validated method on an in-tree node with availability and arity checks.",
			R"({"type":"object","properties":{"node_path":{"type":"string","description":"Path relative to the edited scene root. Default '.'."},"method":{"type":"string","description":"Method name; see class.introspect."},"args":{"type":"array","description":"Positional arguments for the method. Default []."}},"required":["method"]})",
			edit_scene, "method_call", true, false, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->call_method(a); });
	Vector<String> batch_redact;
	batch_redact.push_back("operations");
	_add("objects.batch", "Run validated engine primitives in one round-trip: create_node, set_property, get_property, call_method, reparent, connect_signal, attach_script, remove_node, list_properties, list_signal_connections.",
			R"({"type":"object","properties":{"operations":{"type":"array","description":"Ordered operations. create_node example: {\"op\":\"create_node\",\"class_name\":\"Node3D\",\"name\":\"Tree\",\"parent_path\":\"Forest\"}. Alias parent is accepted. Reparent uses new_parent_path; alias new_parent is accepted."}},"required":["operations"]})",
			edit_scene, "editor_undo_redo", true, true, batch_redact, SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->batch(a); });
	_add("editor.invoke", "Invoke a ClassDB-exposed EditorInterface method such as save_scene or open_scene_from_path. Destructive EditorInterface methods need human approval.",
			R"({"type":"object","properties":{"method":{"type":"string","description":"EditorInterface method name; use class.introspect with class_name=EditorInterface for args."},"args":{"type":"array","description":"Positional JSON arguments. Default []."}},"required":["method"]})",
			edit_scene, "editor_interface", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->invoke_editor(a); });
}

void SolersToolRegistry::_register_search_tools() {
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
	_add_observe("tool.search", "Search additional deferred tools with token-AND matching over registered tool names, descriptions, and schema text. Call discovered tools by canonical name.",
			R"({"type":"object","properties":{"query":{"type":"string","description":"Search tokens. Punctuation and underscores split tokens; every token must match registered deferred tool metadata."},"max_results":{"type":"integer","description":"Maximum tools to return. Default 10."}},"required":["query"]})",
			[this](const SolersToolContext &, const Dictionary &a) {
				const Vector<String> tokens = _search_tokens(String(a.get("query", String())));
				const int max_results = (int)a.get("max_results", 10);
				Array matches;
				Vector<String> names;
				for (const KeyValue<StringName, SolersTool *> &E : tools) {
					if (E.value->exposure() != SolersToolExposure::DEFERRED) {
						continue;
					}
					names.push_back(String(E.key));
				}
				names.sort();
				for (int i = 0; i < names.size(); i++) {
					if (matches.size() >= max_results) {
						break;
					}
					SolersTool *const *tool = tools.getptr(StringName(names[i]));
					if (tool && *tool && _tool_matches_tokens(*tool, tokens)) {
						matches.push_back(_tool_to_dictionary(*tool));
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
	_register(p_tool);
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_register_reflection_tools();
	_register_observation_tools();
	_register_script_tools();
	_register_runtime_tools();
	_register_search_tools();
}

Dictionary SolersToolRegistry::_tool_to_dictionary(const SolersTool *p_tool) const {
	Dictionary tool;
	tool["name"] = String(p_tool->name());
	tool["model_name"] = _make_model_tool_name(p_tool->name());
	tool["description"] = p_tool->description();
	const SolersToolCapability &cap = p_tool->capability();
	tool["permission"] = permission_manager ? permission_manager->get_permission_name(cap.permission) : "observe";
	tool["mutation_kind"] = cap.mutation_kind;
	tool["requires_approval"] = cap.requires_approval;
	tool["undoable"] = cap.undoable;
	tool["exposure"] = _exposure_name(p_tool->exposure());
	tool["input_schema"] = p_tool->parameters_schema();
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

Dictionary SolersToolRegistry::normalize_tool_args(const StringName &p_name, const Dictionary &p_args) const {
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return p_args.duplicate(true);
	}
	return _normalize_tool_args(p_args, (*tool_ptr)->parameters_schema());
}

Dictionary SolersToolRegistry::redact_tool_args_for_fingerprint(const StringName &p_name, const Dictionary &p_args) const {
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
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return _error("TOOL_NOT_FOUND", vformat("Solers tool not found: %s", p_name), true);
	}
	SolersTool *tool = *tool_ptr;
	const SolersToolCapability &cap = tool->capability();
	const Dictionary args = normalize_tool_args(p_name, p_args);
	SolersPermissionManager::Permission effective_permission = cap.permission;

	const Dictionary timeline_args = redact_tool_args_for_fingerprint(p_name, args);
	Dictionary timeline_payload;
	timeline_payload["tool"] = p_name;
	timeline_payload["args"] = timeline_args;
	timeline_payload["permission"] = permission_manager ? permission_manager->get_permission_name(effective_permission) : "observe";
	if (action_timeline) {
		action_timeline->record_event("tool_call_started", timeline_payload);
	}

	if (!permission_manager) {
		return _error("PERMISSION_MANAGER_UNAVAILABLE", "Solers permission manager is not initialized.", false);
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
		return denied;
	}

	SolersToolContext ctx;
	ctx.call_id = args.get("call_id", String());
	ctx.approval_id = approval_id;
	SOLERS_TRACE("registry.execute_begin", vformat("%s args=%s", String(p_name), _trace_json(summarize_tool_args_for_audit(p_name, args), 420)));
	const Dictionary result = tool->execute(ctx, args);
	SOLERS_TRACE("registry.execute_end", vformat("%s %s", String(p_name), summarize_tool_result_for_audit(result)));

	if (action_timeline) {
		Dictionary completed_payload = timeline_payload;
		completed_payload["ok"] = result.get("ok", false);
		action_timeline->record_event("tool_call_completed", completed_payload);
	}

	return result;
}

int SolersToolRegistry::get_tool_count() const {
	return tools.size();
}

SolersToolRegistry::SolersToolRegistry() {}

SolersToolRegistry::~SolersToolRegistry() {
	_clear_tools();
}
