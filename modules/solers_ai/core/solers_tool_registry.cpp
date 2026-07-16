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
#include "modules/solers_ai/core/solers_asset_service.h"
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

static bool _tool_text_matches_any_token(const String &p_text, const Vector<String> &p_tokens) {
	if (p_tokens.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_tokens.size(); i++) {
		if (p_text.contains(p_tokens[i])) {
			return true;
		}
	}
	return false;
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
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler, bool p_ephemeral_result,
		SolersToolExecution p_execution, std::function<Array(const Dictionary &)> p_resource_access,
		bool p_cache_across_revisions, SolersFunctionTool::PollHandler p_poll_handler, bool p_produces_scene_validation, SolersFunctionTool::ReadyHandler p_ready_handler, SolersFunctionTool::CompletionHandler p_completion_handler) {
	SolersToolCapability cap;
	cap.permission = p_permission;
	cap.mutation_kind = p_mutation_kind ? String::utf8(p_mutation_kind) : String("none");
	cap.requires_approval = p_requires_approval;
	cap.undoable = p_undoable;
	cap.ephemeral_result = p_ephemeral_result;
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
		SolersToolExposure p_exposure, SolersFunctionTool::Handler p_handler, bool p_ephemeral_result,
		std::function<Array(const Dictionary &)> p_resource_access, bool p_cache_across_revisions, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler) {
	_add(p_name, p_description, p_schema_json, SolersPermissionManager::PERMISSION_OBSERVE, "none",
			/*requires_approval*/ false, /*undoable*/ false, Vector<String>(), p_exposure, std::move(p_handler), p_ephemeral_result,
			SolersToolExecution::MAIN_THREAD, std::move(p_resource_access), p_cache_across_revisions, std::move(p_poll_handler), false, std::move(p_ready_handler));
}

void SolersToolRegistry::_add_observe(const char *p_name, const char *p_description, const char *p_schema_json,
		SolersFunctionTool::Handler p_handler, std::function<Array(const Dictionary &)> p_resource_access, SolersFunctionTool::PollHandler p_poll_handler, SolersFunctionTool::ReadyHandler p_ready_handler) {
	_add_observe_exposed(p_name, p_description, p_schema_json, SolersToolExposure::DIRECT, std::move(p_handler), true, std::move(p_resource_access), false, std::move(p_poll_handler), std::move(p_ready_handler));
}

void SolersToolRegistry::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
}

void SolersToolRegistry::set_asset_service(SolersAssetService *p_asset_service) {
	asset_service = p_asset_service;
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

// Idempotent playback control that always reports the authoritative
// post-state. Requesting a state the editor is already in is a no-op success,
// so a replayed or duplicated call can never spawn a second game instance.
Dictionary SolersToolRegistry::_run_control(const Dictionary &p_args) const {
	EditorInterface *editor_interface = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(editor_interface, _error("EDITOR_INTERFACE_UNAVAILABLE", "EditorInterface is not available.", false));

	const String action = p_args.get("action", String());
	const bool was_playing = editor_interface->is_playing_scene();
	if (action == "play_current_scene") {
		if (!was_playing) {
			editor_interface->play_current_scene();
		}
	} else if (action == "stop") {
		if (was_playing) {
			editor_interface->stop_playing_scene();
		}
	} else {
		return _error("INVALID_ARGUMENT", "action must be play_current_scene or stop.");
	}

	Dictionary data;
	data["action"] = action;
	data["is_playing"] = editor_interface->is_playing_scene();
	data["playing_scene"] = editor_interface->get_playing_scene();
	data["started_by_call"] = action == "play_current_scene" && !was_playing && (bool)data["is_playing"];
	data["stopped_by_call"] = action == "stop" && was_playing && !(bool)data["is_playing"];
	if (action == "play_current_scene" && was_playing) {
		data["already_playing"] = true;
	}
	return _ok(data);
}

void SolersToolRegistry::_register_observation_tools() {
	if (!observation_service) {
		return;
	}
	SolersObservationService *obs = observation_service;

	_add_observe_exposed("project.search_files", "Search project file paths under res:// by case-insensitive substring; omit query to list a bounded file index.",
			R"({"type":"object","properties":{"query":{"type":"string","description":"Optional case-insensitive substring matched against res:// paths. Empty lists bounded project files."},"max_files":{"type":"integer","description":"Maximum number of matches to return (1-2000). Default 128."}}})",
			SolersToolExposure::DIRECT,
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->search_project_files(a.get("query", String()), (int)a.get("max_files", 128))); });
	_add_observe("project.read_file", "Read a project file from res:// with project-root boundary and byte limits.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to read."},"max_bytes":{"type":"integer","description":"Maximum bytes to return. Default 262144."}},"required":["path"]})",
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->read_project_file(a.get("path", String()), (int)a.get("max_bytes", 262144))); },
			_access_by_arg("read", "project:", "path"));
	_add_observe("editor.get_snapshot", "Read a combined project, scene, selection, and runtime snapshot.",
			R"({"type":"object","properties":{"max_scene_depth":{"type":"integer","description":"Maximum scene-tree depth to serialize. Default 4."},"max_children_per_node":{"type":"integer","description":"Maximum children serialized per node. Default 64."},"include_remote_scene":{"type":"boolean","description":"When true and the game is running, include the debugger remote scene tree. Default false."}}})",
			[this, obs](const SolersToolContext &, const Dictionary &a) { return _ok(obs->get_editor_snapshot((int)a.get("max_scene_depth", 4), (int)a.get("max_children_per_node", 64), (bool)a.get("include_remote_scene", false))); });
	_add_observe_exposed("viewport.capture", "Capture explicit visual evidence. orthographic and top_down use a transient Camera3D/SubViewport sharing the live World3D; focus_paths frames exact scene subtrees and section_position places an explicit world-space cut plane without hiding or mutating nodes. Captures are frame-gated and content-addressed.",
			R"({"type":"object","properties":{"target":{"type":"string","enum":["editor","camera","top_down","orthographic","runtime"],"description":"Viewport source to capture."},"node_path":{"type":"string","description":"For target=camera: path of the Camera3D to render through. Defaults to the first camera in the scene."},"axis":{"type":"string","enum":["x","y","z"],"description":"For target=orthographic: world view axis."},"direction":{"type":"string","enum":["positive","negative"],"description":"For target=orthographic: side of the world axis from which the camera looks."},"focus_paths":{"type":"array","items":{"type":"string"},"description":"Optional edited-scene subtrees whose visible geometry defines framing."},"section_position":{"type":"number","description":"Optional world-space camera cut position on the view axis, useful for looking inside enclosed rooms."}},"required":["target"]})",
			SolersToolExposure::DEFERRED,
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->capture_viewport(a); }, true, {}, false,
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->poll_viewport_capture(a); },
			[obs](const SolersToolContext &, const Dictionary &a) { return obs->is_viewport_capture_ready(a); });

	if (resource_service) {
		SolersResourceService *svc = resource_service;
		_add_observe_exposed("resource.get_info", "Read resource metadata and, for Mesh/PackedScene resources, native root-relative geometry facts: AABB in meters, nested transforms, mesh/surface/triangle counts, and UV2 coverage.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the resource."},"include_dependencies":{"type":"boolean","description":"Include dependency list. Default true."},"max_dependencies":{"type":"integer","description":"Maximum dependencies to return (0-2048). Default 128."}},"required":["path"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->get_resource_info(a); }, true,
				_access_by_arg("read", "project:", "path"));
		_add("resource.create", "Instantiate a Godot Resource subclass, apply its initial native properties, and save it once.",
				R"({"type":"object","properties":{"class_name":{"type":"string","description":"Instantiable Resource class from class.introspect."},"path":{"type":"string","description":"res:// path to save."},"properties":{"type":"object","description":"Optional initial native properties. Math types accept ordered arrays or named component objects such as Vector3 {x,y,z} and Color {r,g,b,a}; Object properties accept res:// resource paths."}},"required":["class_name","path"]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, "resource_save", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->create_resource(a); }, false,
				SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
		_add_observe_exposed("resource.get_property", "Load a res:// Resource with ResourceLoader and read one native Object property.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// resource path."},"property":{"type":"string","description":"Native property name."},"type_hint":{"type":"string","description":"Optional ResourceLoader type hint."}},"required":["path","property"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->get_resource_property(a); }, true,
				_access_by_arg("read", "project:", "path"));
		_add("resource.set_property", "Load a res:// Resource, atomically set one or more native properties, verify them, and save once. Prefer properties when configuring a material.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// resource path."},"properties":{"type":"object","description":"One or more native properties to set atomically. Math types accept ordered arrays or named component objects such as Vector3 {x,y,z} and Color {r,g,b,a}; Object properties accept res:// resource paths."},"property":{"type":"string","description":"Single-property compatibility form; use together with value."},"value":{"description":"Single-property compatibility value."},"type_hint":{"type":"string","description":"Optional ResourceLoader type hint."}},"required":["path"]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, "resource_save", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->set_resource_property(a); }, false,
				SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
		_add("native.instantiate", "Instantiate any ClassDB RefCounted type and return a live handle. Use this for native helpers such as SurfaceTool or MeshDataTool; Nodes belong in objects.batch.",
				R"({"type":"object","properties":{"class_name":{"type":"string","description":"Any ClassDB-instantiable RefCounted class from class.search/class.introspect."}},"required":["class_name"]})",
				SolersPermissionManager::PERMISSION_EDIT_SCENE, "native_instantiate", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_instantiate(a); });
		_add_observe_exposed("native.load", "Load any res:// Resource and return a live Godot object handle for native.get/native.call.",
				R"({"type":"object","properties":{"path":{"type":"string","description":"res:// resource path."},"type_hint":{"type":"string","description":"Optional ResourceLoader type hint."}},"required":["path"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_load(a); });
		_add_observe_exposed("native.list_properties", "List the live Godot Object properties for a handle returned by native.load/native.call/editor.invoke.",
				R"({"type":"object","properties":{"object_id":{"description":"object_id or full Godot object handle."}},"required":["object_id"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_list_properties(a); });
		_add_observe_exposed("native.get", "Read one property from a live Godot Object handle.",
				R"({"type":"object","properties":{"object_id":{"description":"object_id or full Godot object handle."},"property":{"type":"string","description":"Native property name."}},"required":["object_id","property"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_get(a); });
		_add("native.set", "Set one property on a live Godot Object handle.",
				R"({"type":"object","properties":{"object_id":{"description":"object_id or full Godot object handle."},"property":{"type":"string","description":"Native property name."},"value":{"description":"New value. Math types accept ordered arrays or named component objects such as Vector3 {x,y,z} and Color {r,g,b,a}; Object properties accept res:// resource paths."}},"required":["object_id","property","value"]})",
				SolersPermissionManager::PERMISSION_EDIT_SCENE, "native_set", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_set(a); });
		_add_observe_exposed("native.list_methods", "List methods exposed by a live Godot Object handle.",
				R"({"type":"object","properties":{"object_id":{"description":"object_id or full Godot object handle."}},"required":["object_id"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_list_methods(a); });
		_add("native.call", "Call one method on a live Godot Object handle. Use class.introspect/native.list_methods first when unsure.",
				R"({"type":"object","properties":{"object_id":{"description":"object_id or full Godot object handle."},"method":{"type":"string","description":"Native method name."},"args":{"type":"array","description":"Positional arguments. Default []. Values are converted from the live MethodInfo signature; object handles and res:// resources may be passed directly."}},"required":["object_id","method"]})",
				SolersPermissionManager::PERMISSION_EDIT_SCENE, "native_call", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_call(a); });
		_add("native.save", "Save a live Resource handle to one explicit res:// path with ResourceSaver.",
				R"({"type":"object","properties":{"object_id":{"description":"Resource object_id or full Godot object handle."},"path":{"type":"string","description":"Explicit res:// destination path."}},"required":["object_id","path"]})",
				SolersPermissionManager::PERMISSION_EDIT_FILES, "resource_save", true, false, Vector<String>(), SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_save(a); }, false,
				SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
		_add_observe_exposed("native.free", "Release a handle retained by native.instantiate/native.load/native.call or a temporary detached node.",
				R"({"type":"object","properties":{"object_id":{"description":"object_id or full Godot object handle."}},"required":["object_id"]})",
				SolersToolExposure::DIRECT,
				[svc](const SolersToolContext &, const Dictionary &a) { return svc->native_free(a); });
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
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to write."},"content":{"type":"string","description":"Full new text content. Mutually exclusive with content_base64. Use script.patch for existing scripts."},"content_base64":{"type":"string","description":"Base64 raw bytes for binary assets. Mutually exclusive with content."},"create":{"type":"boolean","description":"Create the file when missing. Default true."},"overwrite":{"type":"boolean","description":"Overwrite existing content. Default true."}},"required":["path"]})";

	Vector<String> file_write_redact;
	file_write_redact.push_back("content");
	file_write_redact.push_back("content_base64");
	_add("project.write_file", "Create or overwrite a non-resource project text/binary file. Godot serialized scenes/resources require native scene and Resource tools. Existing scripts should be edited with script.patch; script validation is mandatory.", file_write_schema,
			edit_files, "file_write", true, false, file_write_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->write_file(a); }, false,
			SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
	Vector<String> file_patch_redact;
	file_patch_redact.push_back("old_text");
	file_patch_redact.push_back("new_text");
	_add("script.patch", "Apply an exact text replacement to a script or text file with optional sha256 guard, checkpointing, and mandatory script validation.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the file to patch."},"old_text":{"type":"string","description":"Exact existing text to replace."},"new_text":{"type":"string","description":"Replacement text."},"occurrence":{"type":"integer","description":"1-based occurrence of old_text to replace. Default 1."},"expected_sha256":{"type":"string","description":"Optional sha256 the current file content must match."}},"required":["path","old_text","new_text"]})",
			edit_files, "file_patch", true, false, file_patch_redact, SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->patch_file(a); }, false,
			SolersToolExecution::MAIN_THREAD, _access_by_arg("write", "project:", "path"));
	_add_observe_exposed("script.validate", "Validate script source through Godot's registered ScriptLanguage implementation.",
			R"({"type":"object","properties":{"path":{"type":"string","description":"res:// path of the script to validate."},"source":{"type":"string","description":"Optional source override; validates this text instead of the file content."}},"required":["path"]})",
			SolersToolExposure::DIRECT,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->validate_script(a); });
}

void SolersToolRegistry::_register_runtime_tools() {
	const SolersPermissionManager::Permission run_project = SolersPermissionManager::PERMISSION_RUN_PROJECT;
	_add("runtime.control", "Run primitive for controlling editor playback. action=play_current_scene starts the edited scene; action=stop stops playback. Idempotent: repeating an action reports the authoritative post-state (is_playing, already_playing) instead of starting a second instance.",
			R"({"type":"object","properties":{"action":{"type":"string","description":"play_current_scene or stop."}},"required":["action"]})",
			run_project, "runtime_only", true, false, Vector<String>(), SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) { return _run_control(a); }, false,
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "write";
				access["key"] = "runtime:";
				accesses.push_back(access);
				return accesses;
			});
}

void SolersToolRegistry::_register_asset_tools() {
	if (!asset_service) {
		return;
	}
	SolersAssetService *svc = asset_service;
	_add("asset.catalog.search", "Search cached lightweight metadata from one official CC0 catalog. ambientCG and Poly Haven both support materials, HDRIs, and 3D models. Terms use coverage matching instead of strict AND; results explain matched_terms and matched_fields. Search never resolves files or downloads previews. Inspect one selected result before acquire.",
			R"({"type":"object","properties":{"provider":{"type":"string","enum":["ambientcg","polyhaven"],"description":"Official catalog to search."},"query":{"type":"string","description":"Material, HDRI, or 3D model search terms. Multiple terms broaden candidate coverage and improve ranking; they do not all have to match."},"kind":{"type":"string","enum":["material","hdri","3d"],"description":"Asset kind supported by both catalogs."},"limit":{"type":"integer","description":"Maximum returned results; default 20, maximum 50."},"offset":{"type":"integer","description":"Zero-based result offset."},"refresh":{"type":"boolean","description":"Explicitly refresh the provider's cached lightweight directory."}},"required":["provider","query","kind"]})",
			SolersPermissionManager::PERMISSION_NETWORK, "asset_catalog_search", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_search(a, ctx.cancel_requested); }, false, SolersToolExecution::WORKER_THREAD,
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
			SolersPermissionManager::PERMISSION_NETWORK, "asset_catalog_inspect", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_inspect(a, ctx.cancel_requested); }, false, SolersToolExecution::WORKER_THREAD,
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
			SolersPermissionManager::PERMISSION_NETWORK, "asset_generate", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->generate_for_session(a, ctx.session_id); });
	_add("asset.catalog.acquire", "Queue one exact variant returned by asset.catalog.inspect. The task revalidates official metadata, downloads the selected package, verifies available checksums, and stores licensing and attribution under one stable job id.",
			R"({"type":"object","properties":{"provider":{"type":"string","enum":["ambientcg","polyhaven"],"description":"Provider returned by asset.catalog.inspect."},"kind":{"type":"string","enum":["material","hdri","3d"],"description":"Kind returned by asset.catalog.inspect."},"asset_id":{"type":"string","description":"Exact official provider asset id."},"variant":{"type":"string","description":"Exact variants[].id returned by asset.catalog.inspect; never guess a format."},"source_version":{"type":"string","description":"Exact source_version returned by asset.catalog.inspect."},"name":{"type":"string","description":"Optional local display name."}},"required":["provider","kind","asset_id","variant","source_version"]})",
			SolersPermissionManager::PERMISSION_NETWORK, "asset_catalog_acquire", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->catalog_acquire(a, ctx.session_id); }, false,
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
			SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->capabilities(a); });
	_add("asset.run_operation", "Queue one available operation on an existing Solers Library asset. The operation creates a new derived asset, never overwrites the source, and Solers resumes this Agent session when it reaches a terminal state.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id."},"operation_id":{"type":"string","description":"Operation id returned by asset.capabilities."},"options":{"type":"object","description":"Operation options that match the schema returned by asset.capabilities."},"raw_provider_options":{"type":"object","description":"Advanced provider-native options. Requires raw_confirmed=true."},"raw_confirmed":{"type":"boolean","description":"Set true only after the user explicitly confirms raw provider options."}},"required":["asset_id","operation_id"]})",
			SolersPermissionManager::PERMISSION_NETWORK, "asset_operation", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->run_operation_for_session(a, ctx.session_id); });
	_add_observe_exposed("asset.status", "Read a local asset generation task or generated asset manifest by asset_id.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id returned by asset.generate or asset.list_local."}},"required":["asset_id"]})",
			SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->status(a); }, true,
			_access_by_arg("read", "asset:", "asset_id"));
	_add_observe_exposed("job.wait", "Declare the background asset job ids needed before the Agent can continue. This returns their current terminal or pending states immediately. Keep doing conflict-free work when possible; when nothing remains, call this once and stop issuing tools so the same Agent turn waits until any requested job reaches a terminal state.",
			R"({"type":"object","properties":{"ids":{"type":"array","items":{"type":"string"},"description":"Stable job ids returned by asset.generate, asset.catalog.acquire, or asset.run_operation."}},"required":["ids"]})",
			SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &ctx, const Dictionary &a) { return svc->wait_jobs(a, ctx.session_id); }, false,
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
			SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->list_local(a); }, true,
			[](const Dictionary &) {
				Array accesses;
				Dictionary access;
				access["mode"] = "read";
				access["key"] = "asset-library:";
				accesses.push_back(access);
				return accesses;
			});
	_add("asset.import_to_project", "Copy a ready Solers Library asset into res://, then wait until Godot finishes importing every selected dependency and the resources can actually be loaded. Static 3D models use Godot's native Static Lightmaps importer mode and report actual mesh/triangle/UV2 statistics; generated assets that exceed their requested topology budget are rejected before placement. Texture-set materials require map_types chosen from the manifest's exact map roles, so unused alternatives are not imported.",
			R"({"type":"object","properties":{"asset_id":{"type":"string","description":"Local Solers asset id."},"target_dir":{"type":"string","description":"Optional res:// destination directory. Defaults to res://solers_assets/<kind>/<name>."},"map_types":{"type":"array","items":{"type":"string"},"description":"For texture-set materials, exact map role names from manifest.map_files that the final material will reference. Omit for native materials, HDRIs, and 3D assets."}},"required":["asset_id"]})",
			SolersPermissionManager::PERMISSION_EDIT_FILES, "asset_import", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->import_to_project(a); }, false,
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
			[svc](const SolersToolContext &, const Dictionary &a) { return svc->is_project_import_ready(a); });
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
				Array required_tools;
				for (const String &tool : skill.required_tools) {
					required_tools.push_back(tool);
				}
				data["required_tools"] = required_tools;
				return _ok(data);
			}, false);
}

void SolersToolRegistry::_register_reflection_tools() {
	if (!reflection_service) {
		return;
	}
	SolersReflectionService *ref = reflection_service;
	const SolersPermissionManager::Permission edit_scene = SolersPermissionManager::PERMISSION_EDIT_SCENE;
	_add("scene.instantiate", "Instantiate one imported or authored PackedScene or Mesh through Godot ResourceLoader and UndoRedo. Returns the live path plus world-space AABB, transform, lowest point, mesh statistics, and UV2 coverage so scale and placement can be measured rather than guessed.",
			R"({"type":"object","properties":{"resource_path":{"type":"string","description":"Loadable res:// PackedScene or Mesh entrypoint returned by asset.import_to_project or project inspection."},"parent_path":{"type":"string","description":"Parent path relative to the edited scene root. Default '.'."},"name":{"type":"string","description":"Optional root node name."},"properties":{"type":"object","description":"Optional native properties applied to the instantiated root before insertion. Math types accept ordered arrays or named component objects such as Vector3 {x,y,z}."}},"required":["resource_path"]})",
			edit_scene, "editor_undo_redo", true, true, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->instantiate_scene(a); }, false,
			SolersToolExecution::MAIN_THREAD, [](const Dictionary &a) {
				Array accesses;
				Dictionary resource;
				resource["mode"] = "read";
				resource["key"] = "project:" + String(a.get("resource_path", String())).replace_char('\\', '/').simplify_path();
				accesses.push_back(resource);
				Dictionary scene;
				scene["mode"] = "write";
				scene["key"] = "scene:" + String(a.get("parent_path", "."));
				accesses.push_back(scene);
				return accesses;
			});
	_add("scene.validate_spatial", "Diagnose declared world-space contacts, projection coverage, shared datums, and forbidden overlaps against live Godot AABBs. This checks only the supplied relations and does not satisfy the structural completion gate.",
			R"({"type":"object","properties":{"relations":{"type":"array","description":"Spatial contracts to measure.","items":{"type":"object","properties":{"a":{"type":"string","description":"First GeometryInstance3D node path."},"b":{"type":"string","description":"Second GeometryInstance3D node path."},"kind":{"type":"string","enum":["max_gap","contains","align","no_overlap"]},"axes":{"type":"array","items":{"type":"string","enum":["x","y","z"]},"description":"Axes for max_gap, contains, or no_overlap; omit for all axes."},"axis":{"type":"string","enum":["x","y","z"],"description":"Single datum axis required by align."},"a_anchor":{"type":"string","enum":["min","center","max"],"description":"First bounds anchor for align; default center."},"b_anchor":{"type":"string","enum":["min","center","max"],"description":"Second bounds anchor for align; default center."},"tolerance":{"type":"number","description":"Explicit non-negative tolerance in project units; for no_overlap this is the maximum allowed penetration."}},"required":["a","b","kind","tolerance"]}}},"required":["relations"]})",
			SolersPermissionManager::PERMISSION_OBSERVE, "none", false, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->validate_spatial_relations(a); }, true);
	_add("scene.validate_structure", "Validate complete architecture and optional physical support/reference-image layout using live Godot Mesh/CSG geometry in meters. Omit placement_roots and placements when support validation is not requested. reference_layout records explicit measurable constraints and returns the exact orthographic evidence views required.",
			R"({"type":"object","properties":{"structure_roots":{"type":"array","minItems":1,"items":{"type":"string"},"description":"Every non-overlapping root containing architectural structure or fixed finish geometry."},"relations":{"type":"array","items":{"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"},"kind":{"type":"string","enum":["max_gap","contains","align","no_overlap"]},"axes":{"type":"array","items":{"type":"string","enum":["x","y","z"]}},"axis":{"type":"string","enum":["x","y","z"]},"a_anchor":{"type":"string","enum":["min","center","max"]},"b_anchor":{"type":"string","enum":["min","center","max"]}},"required":["a","b","kind"]}},"placement_roots":{"type":"array","items":{"type":"string"},"description":"Optional roots containing physical props/fixtures. Must be supplied together with placements."},"placements":{"type":"array","description":"Optional support relations. Must be supplied together with placement_roots.","items":{"type":"object","properties":{"member":{"type":"string"},"supported_by":{"type":"string"},"axis":{"type":"string","enum":["x","y","z"]},"direction":{"type":"string","enum":["positive","negative"]},"tolerance":{"type":"number"}},"required":["member","supported_by","axis","direction","tolerance"]}},"reference_layout":{"type":"object","description":"Required for reference-image tasks. Measurements are in meters and bind to the exact attachment.","properties":{"attachment_id":{"type":"string"},"constraints":{"type":"array","minItems":1,"items":{"type":"object","properties":{"kind":{"type":"string","enum":["extent","anchor_distance","axis_alignment"]},"member":{"type":"string"},"a":{"type":"string"},"b":{"type":"string"},"axis":{"type":"string","enum":["x","y","z"]},"a_anchor":{"type":"string","enum":["min","center","max"]},"b_anchor":{"type":"string","enum":["min","center","max"]},"min":{"type":"number"},"max":{"type":"number"},"local_axis":{"type":"string","enum":["x","y","z"]},"world_axis":{"type":"string","enum":["x","y","z"]},"direction":{"type":"integer","enum":[-1,1]},"max_angle_degrees":{"type":"number"}},"required":["kind"]}}},"required":["attachment_id","constraints"]}},"required":["structure_roots","relations"]})",
			SolersPermissionManager::PERMISSION_OBSERVE, "none", false, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->validate_structure(a); }, true,
			SolersToolExecution::MAIN_THREAD, {}, false, {}, true);
	_add("scene.bake_csg", "Bake exact CSG root node paths directly through Godot's CSG API into static MeshInstance3D artifacts. This operation does not depend on editor selection.",
			R"({"type":"object","properties":{"node_paths":{"type":"array","minItems":1,"items":{"type":"string"}},"hide_sources":{"type":"boolean","description":"Hide source CSG roots after a successful atomic bake. Default true."}},"required":["node_paths"]})",
			edit_scene, "editor_undo_redo", true, true, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_csg(a); });
	_add("mesh.unwrap_uv2", "Prepare UV2 for exact MeshInstance3D paths through Godot's native mesh API. ArrayMesh work runs one mesh at a time off the editor thread, reports progress, and commits one atomic UndoRedo action only after every surface verifies ARRAY_FORMAT_TEX_UV2. Imported static models should normally arrive with UV2 from Godot's Static Lightmaps importer mode.",
			R"({"type":"object","properties":{"node_paths":{"type":"array","minItems":1,"items":{"type":"string"}}},"required":["node_paths"]})",
			edit_scene, "editor_undo_redo", true, true, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &ctx, const Dictionary &a) { return ref->unwrap_uv2(a, ctx.call_id); }, false,
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
			edit_scene, "lightmap_bake", true, false, Vector<String>(), SolersToolExposure::DEFERRED,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->bake_lightmap(a); });

	_add_observe_exposed("class.search", "Search Godot ClassDB by class name and optional inheritance before introspection. Returns real class names and whether each is a Node or Resource.",
			R"({"type":"object","properties":{"query":{"type":"string","description":"Case-insensitive class-name fragment, e.g. CSG, Material, Light."},"inherits":{"type":"string","description":"Optional ClassDB ancestor filter, e.g. Node3D, Resource."},"max_results":{"type":"integer","description":"Maximum results to return. Default 40, max 200."}}})",
			SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->search_classes(a); }, true, {}, true);
	_add_observe_exposed("class.introspect", "Introspect the current engine's ClassDB signatures and integrated EditorHelp documentation. member_query narrows members and includes their native descriptions.",
			R"({"type":"object","properties":{"class_name":{"type":"string","description":"Engine class to introspect, e.g. Node3D, Sprite2D, CharacterBody2D."},"include_inherited":{"type":"boolean","description":"Include members inherited from parent classes. Default true."},"member_query":{"type":"string","description":"Optional case-insensitive member name or documentation fragment. When set, only matching methods, properties, signals, and constants are returned with native descriptions."}},"required":["class_name"]})",
			SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->introspect_class(a); }, true, {}, true);
	_add_observe_exposed("object.get_property", "Read one ClassDB-validated property off an in-tree node in the edited scene.",
			R"({"type":"object","properties":{"node_path":{"type":"string","description":"Path relative to the edited scene root. Default '.'."},"property":{"type":"string","description":"Property name; see class.introspect for valid names."}},"required":["property"]})",
			SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->get_property(a); });
	_add("object.set_property", "Write one ClassDB-validated, type-coerced property on an in-tree node through EditorUndoRedoManager (undoable). Math types accept ordered arrays or named component objects; Object/Resource properties accept res:// resource paths.",
			R"({"type":"object","properties":{"node_path":{"type":"string","description":"Path relative to the edited scene root. Default '.'."},"property":{"type":"string","description":"Property name; see class.introspect."},"value":{"description":"New value. Pass numbers/strings/bools directly; pass math types as ordered arrays or named component objects such as Vector3 {x,y,z}; pass res:// paths for Object/Resource properties."}},"required":["property","value"]})",
			edit_scene, "editor_undo_redo", true, true, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->set_property(a); });
	_add("object.call_method", "Call a ClassDB-validated method on an in-tree node with availability and arity checks.",
			R"({"type":"object","properties":{"node_path":{"type":"string","description":"Path relative to the edited scene root. Default '.'."},"method":{"type":"string","description":"Method name; see class.introspect."},"args":{"type":"array","description":"Positional arguments for the method. Default []."}},"required":["method"]})",
			edit_scene, "method_call", true, false, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->call_method(a); });
	_add("objects.batch", "Run validated undoable engine primitives atomically in one round-trip: create_node, set_property, get_property, reparent, connect_signal, attach_script, remove_node, list_properties, list_signal_connections. In an empty editor scene, the first create_node with parent_path='.' becomes the scene root.",
			R"({"type":"object","properties":{"operations":{"type":"array","minItems":1,"description":"Ordered operations. Node targets use node_path relative to the edited scene root. create_node uses parent_path and optional native properties; reparent uses new_parent_path. Math values accept ordered arrays or named component objects such as Vector3 {x,y,z}.","items":{"type":"object","properties":{"op":{"type":"string","enum":["create_node","set_property","get_property","reparent","connect_signal","attach_script","remove_node","list_properties","list_signal_connections"]},"class_name":{"type":"string"},"name":{"type":"string"},"parent_path":{"type":"string"},"properties":{"type":"object"},"node_path":{"type":"string"},"property":{"type":"string"},"value":{},"new_parent_path":{"type":"string"},"position":{"type":"integer"},"source_path":{"type":"string"},"signal":{"type":"string"},"target_path":{"type":"string"},"method":{"type":"string"},"flags":{"type":"integer"},"script_path":{"type":"string"},"max_properties":{"type":"integer"}},"required":["op"]}}},"required":["operations"]})",
			edit_scene, "editor_undo_redo", true, true, Vector<String>(), SolersToolExposure::DIRECT,
			[ref](const SolersToolContext &, const Dictionary &a) { return ref->batch(a); }, true, SolersToolExecution::MAIN_THREAD,
			[ref](const Dictionary &a) { return ref->resolve_batch_resource_access(a); });
	_add("editor.invoke", "Invoke a ClassDB-exposed EditorInterface method such as save_scene or open_scene_from_path. Prefer this native editor API before writing code; destructive methods need human approval.",
			R"({"type":"object","properties":{"method":{"type":"string","description":"EditorInterface method name; use class.introspect with class_name=EditorInterface for args."},"args":{"type":"array","description":"Positional JSON arguments. Default []."}},"required":["method"]})",
			edit_scene, "editor_interface", true, false, Vector<String>(), SolersToolExposure::DIRECT,
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
	_add_observe_exposed("tool.search", "Search additional deferred tools with broad token matching over registered tool names, descriptions, and schema text. Name matches are returned first; discovered tools remain available for the session.",
			R"({"type":"object","properties":{"query":{"type":"string","description":"Search terms. Punctuation and underscores split terms; any matching term may return a tool, with tool-name matches first."},"max_results":{"type":"integer","description":"Maximum tools to return. Default 10."}},"required":["query"]})",
			SolersToolExposure::DIRECT,
			[this](const SolersToolContext &, const Dictionary &a) {
				const Vector<String> tokens = _search_tokens(String(a.get("query", String())));
				const int max_results = (int)a.get("max_results", 10);
				Array matches;
				Vector<String> name_matches;
				Vector<String> metadata_matches;
				for (const KeyValue<StringName, SolersTool *> &E : tools) {
					if (E.value->exposure() != SolersToolExposure::DEFERRED) {
						continue;
					}
					const String name = String(E.key);
					if (_tool_text_matches_any_token(name.to_lower(), tokens)) {
						name_matches.push_back(name);
					} else if (_tool_text_matches_any_token(_tool_search_corpus(E.value), tokens)) {
						metadata_matches.push_back(name);
					}
				}
				name_matches.sort();
				metadata_matches.sort();
				for (int group = 0; group < 2 && matches.size() < max_results; group++) {
					const Vector<String> &names = group == 0 ? name_matches : metadata_matches;
					for (int i = 0; i < names.size() && matches.size() < max_results; i++) {
						SolersTool *const *tool = tools.getptr(StringName(names[i]));
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
	_register(p_tool);
}

void SolersToolRegistry::register_default_tools() {
	_clear_tools();
	_register_skill_tools();
	_register_reflection_tools();
	_register_observation_tools();
	_register_script_tools();
	_register_runtime_tools();
	_register_asset_tools();
	_register_search_tools();
}

void SolersToolRegistry::validate_builtin_skills() const {
	HashSet<StringName> registered_tools;
	for (const KeyValue<StringName, SolersTool *> &E : tools) {
		registered_tools.insert(E.key);
	}
	const Vector<String> missing = SolersBuiltinSkills::validate_required_tools(registered_tools);
	for (int i = 0; i < missing.size(); i++) {
		ERR_PRINT(missing[i]);
	}
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
	tool["ephemeral_result"] = cap.ephemeral_result;
	tool["produces_scene_validation"] = cap.produces_scene_validation;
	tool["execution"] = cap.execution == SolersToolExecution::WORKER_THREAD ? "worker" : "main_thread";
	tool["exposure"] = _exposure_name(p_tool->exposure());
	Dictionary input_schema = p_tool->parameters_schema().duplicate(true);
	Dictionary properties = input_schema.get("properties", Dictionary());
	Dictionary retry_of;
	retry_of["type"] = "string";
	retry_of["description"] = "Exact error.failure_id returned by the failed operation this call repairs. Do not invent or transform this value.";
	properties["retry_of"] = retry_of;
	input_schema["properties"] = properties;
	tool["input_schema"] = input_schema;
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

bool SolersToolRegistry::is_result_ephemeral(const StringName &p_name) const {
	SolersTool *const *tool = tools.getptr(p_name);
	return tool && *tool && (*tool)->capability().ephemeral_result;
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
	return permission == SolersPermissionManager::PERMISSION_EDIT_SCENE || permission == SolersPermissionManager::PERMISSION_EDIT_FILES;
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
		access["mode"] = cap.mutation_kind == "none" ? "read" : "write";
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

Dictionary SolersToolRegistry::_prepare_tool_call(const StringName &p_name, const Dictionary &p_args, const SolersToolContext &p_context, SolersPreparedToolCall &r_call) {
	SolersTool *const *tool_ptr = tools.getptr(p_name);
	if (!tool_ptr || !*tool_ptr) {
		return _error("TOOL_NOT_FOUND", vformat("Solers tool not found: %s", p_name), true);
	}
	SolersTool *tool = *tool_ptr;
	const SolersToolCapability &cap = tool->capability();
	const Dictionary args = normalize_tool_args(p_name, p_args);
	SolersPermissionManager::Permission effective_permission = is_read_only(p_name, args) ? SolersPermissionManager::PERMISSION_OBSERVE : cap.permission;

	const Dictionary timeline_args = redact_tool_args_for_audit(p_name, args);
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

	SolersToolContext ctx = p_context;
	if (ctx.call_id.is_empty()) {
		ctx.call_id = args.get("call_id", String());
	}
	ctx.approval_id = approval_id;
	ctx.retry_of = args.get("retry_of", String());
	Dictionary handler_args = args;
	handler_args.erase("retry_of");
	r_call.tool = tool;
	r_call.name = p_name;
	r_call.args = handler_args;
	r_call.timeline_payload = timeline_payload;
	r_call.context = ctx;
	r_call.execution = cap.execution;
	return Dictionary();
}

Dictionary SolersToolRegistry::_execute_prepared_tool(const SolersPreparedToolCall &p_call) const {
	ERR_FAIL_NULL_V(p_call.tool, _error("TOOL_NOT_FOUND", "Prepared Solers tool is unavailable.", false));
	SOLERS_TRACE("registry.execute_begin", vformat("%s args=%s", String(p_call.name), _trace_json(summarize_tool_args_for_audit(p_call.name, p_call.args), 420)));
	const Dictionary result = p_call.tool->execute(p_call.context, p_call.args);
	SOLERS_TRACE("registry.execute_end", vformat("%s %s", String(p_call.name), summarize_tool_result_for_audit(result)));
	return result;
}

Dictionary SolersToolRegistry::_poll_prepared_tool(const SolersPreparedToolCall &p_call, const Dictionary &p_args) const {
	ERR_FAIL_NULL_V(p_call.tool, _error("TOOL_NOT_FOUND", "Prepared Solers tool is unavailable.", false));
	SOLERS_TRACE("registry.poll_begin", vformat("%s args=%s", String(p_call.name), _trace_json(summarize_tool_args_for_audit(p_call.name, p_args), 420)));
	const Dictionary result = p_call.tool->poll(p_call.context, p_args);
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
}
