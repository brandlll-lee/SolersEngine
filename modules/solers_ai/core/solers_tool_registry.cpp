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

#include "core/object/class_db.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_builtin_tool_catalog.h"
#include "modules/solers_ai/core/solers_editor_operator.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_resource_service.h"
#include "modules/solers_ai/core/solers_script_service.h"
#include "modules/solers_ai/core/solers_settings_service.h"
#include "modules/solers_ai/protocol/solers_rpc_server.h"

void SolersToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_editor_operator", "editor_operator"), &SolersToolRegistry::set_editor_operator);
	ClassDB::bind_method(D_METHOD("set_observation_service", "observation_service"), &SolersToolRegistry::set_observation_service);
	ClassDB::bind_method(D_METHOD("set_resource_service", "resource_service"), &SolersToolRegistry::set_resource_service);
	ClassDB::bind_method(D_METHOD("set_script_service", "script_service"), &SolersToolRegistry::set_script_service);
	ClassDB::bind_method(D_METHOD("set_settings_service", "settings_service"), &SolersToolRegistry::set_settings_service);
	ClassDB::bind_method(D_METHOD("set_permission_manager", "permission_manager"), &SolersToolRegistry::set_permission_manager);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersToolRegistry::set_action_timeline);
	ClassDB::bind_method(D_METHOD("set_rpc_server", "rpc_server"), &SolersToolRegistry::set_rpc_server);
	ClassDB::bind_method(D_METHOD("register_default_tools"), &SolersToolRegistry::register_default_tools);
	ClassDB::bind_method(D_METHOD("list_tools"), &SolersToolRegistry::list_tools);
	ClassDB::bind_method(D_METHOD("get_model_tool_name", "name"), &SolersToolRegistry::get_model_tool_name);
	ClassDB::bind_method(D_METHOD("resolve_model_tool_name", "model_name"), &SolersToolRegistry::resolve_model_tool_name);
	ClassDB::bind_method(D_METHOD("call_tool", "name", "args"), &SolersToolRegistry::call_tool);
	ClassDB::bind_method(D_METHOD("get_tool_count"), &SolersToolRegistry::get_tool_count);
}

Dictionary SolersToolRegistry::_object_schema() const {
	Dictionary schema;
	schema["type"] = "object";
	schema["additionalProperties"] = true;
	return schema;
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

void SolersToolRegistry::_register_tool(const ToolDefinition &p_definition) {
	ToolDefinition definition = p_definition;
	if (definition.model_name.is_empty()) {
		definition.model_name = _make_model_tool_name(definition.name);
	}
	const StringName model_name = StringName(definition.model_name);
	if (model_name_index.has(model_name) && model_name_index[model_name] != definition.name) {
		ERR_FAIL_MSG(vformat("Solers model tool name collision: %s maps to both %s and %s.", definition.model_name, model_name_index[model_name], definition.name));
	}
	tools[definition.name] = definition;
	model_name_index[model_name] = definition.name;
}

Dictionary SolersToolRegistry::_tool_to_dictionary(const ToolDefinition &p_definition) const {
	Dictionary tool;
	tool["name"] = p_definition.name;
	tool["model_name"] = p_definition.model_name;
	tool["description"] = p_definition.description;
	tool["permission"] = permission_manager ? permission_manager->get_permission_name(p_definition.permission) : "observe";
	tool["mutation_kind"] = p_definition.mutation_kind;
	tool["requires_approval"] = p_definition.requires_approval;
	tool["timeout_ms"] = p_definition.timeout_ms;
	tool["input_schema"] = p_definition.input_schema;
	tool["output_schema"] = p_definition.output_schema;
	return tool;
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

void SolersToolRegistry::set_editor_operator(SolersEditorOperator *p_editor_operator) {
	editor_operator = p_editor_operator;
}

void SolersToolRegistry::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
}

void SolersToolRegistry::set_resource_service(SolersResourceService *p_resource_service) {
	resource_service = p_resource_service;
}

void SolersToolRegistry::set_script_service(SolersScriptService *p_script_service) {
	script_service = p_script_service;
}

void SolersToolRegistry::set_settings_service(SolersSettingsService *p_settings_service) {
	settings_service = p_settings_service;
}

void SolersToolRegistry::set_permission_manager(SolersPermissionManager *p_permission_manager) {
	permission_manager = p_permission_manager;
}

void SolersToolRegistry::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
}

void SolersToolRegistry::set_rpc_server(SolersRpcServer *p_rpc_server) {
	rpc_server = p_rpc_server;
}

void SolersToolRegistry::register_default_tools() {
	tools.clear();
	model_name_index.clear();

	const Dictionary object_schema = _object_schema();
	const Vector<SolersBuiltinToolDefinition> definitions = SolersBuiltinToolCatalog::list_tools();
	for (int i = 0; i < definitions.size(); i++) {
		const SolersBuiltinToolDefinition &builtin = definitions[i];
		ToolDefinition tool;
		tool.name = builtin.name;
		tool.description = builtin.description;
		tool.permission = builtin.permission;
		tool.mutation_kind = builtin.mutation_kind;
		tool.requires_approval = builtin.requires_approval;
		tool.input_schema = object_schema;
		tool.output_schema = object_schema;
		_register_tool(tool);
	}
}

Array SolersToolRegistry::list_tools() const {
	Array result;
	Vector<String> names;
	for (const KeyValue<StringName, ToolDefinition> &E : tools) {
		names.push_back(String(E.key));
	}
	names.sort();
	for (int i = 0; i < names.size(); i++) {
		const ToolDefinition *definition = tools.getptr(StringName(names[i]));
		if (definition) {
			result.push_back(_tool_to_dictionary(*definition));
		}
	}
	return result;
}

String SolersToolRegistry::get_model_tool_name(const StringName &p_name) const {
	const ToolDefinition *definition = tools.getptr(p_name);
	if (!definition) {
		return String();
	}
	return definition->model_name;
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

Dictionary SolersToolRegistry::call_tool(const StringName &p_name, const Dictionary &p_args) {
	ToolDefinition *definition = tools.getptr(p_name);
	if (!definition) {
		return _error("TOOL_NOT_FOUND", vformat("Solers tool not found: %s", p_name), true);
	}

	Dictionary timeline_payload;
	timeline_payload["tool"] = p_name;
	Dictionary timeline_args = p_args;
	if (p_name == StringName("provider.set_config") && timeline_args.has("api_key")) {
		timeline_args["api_key"] = "<redacted>";
	}
	if (p_name == StringName("rpc.start") && timeline_args.has("session_token")) {
		timeline_args["session_token"] = "<redacted>";
	}
	timeline_payload["args"] = timeline_args;
	timeline_payload["permission"] = permission_manager ? permission_manager->get_permission_name(definition->permission) : "observe";
	if (action_timeline) {
		action_timeline->record_event("tool_call_started", timeline_payload);
	}

	if (!permission_manager) {
		return _error("PERMISSION_MANAGER_UNAVAILABLE", "Solers permission manager is not initialized.", false);
	}

	const int approval_id = p_args.get("approval_id", 0);
	const bool has_approval = permission_manager->is_auto_approved(definition->permission) || permission_manager->consume_approval(approval_id, p_name);
	if (!has_approval) {
		Dictionary approval_request = permission_manager->request_user_approval(p_name, timeline_args, definition->permission);
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

	const String tool_name = String(p_name);
	const bool editor_operator_tool = tool_name.begins_with("node.") || p_name == StringName("scene.open") || p_name == StringName("scene.create") || p_name == StringName("scene.save") || p_name == StringName("scene.save_as") || p_name == StringName("runtime.play_current_scene") || p_name == StringName("runtime.stop") || p_name == StringName("runtime.capture_screenshot") || p_name == StringName("editor.capture_screenshot") || p_name == StringName("timeline.rollback_last");
	const bool script_tool = p_name == StringName("project.write_file") || tool_name.begins_with("script.") || tool_name.begins_with("validation.");
	const bool settings_tool = tool_name.begins_with("provider.");
	const bool approval_tool = tool_name.begins_with("approvals.");
	const bool rpc_tool = tool_name.begins_with("rpc.");
	const bool resource_tool = tool_name.begins_with("resource.") || tool_name.begins_with("export.");
	const bool observation_tool = p_name != StringName("timeline.list_actions") && !editor_operator_tool && !script_tool && !settings_tool && !approval_tool && !rpc_tool && !resource_tool;
	if (observation_tool && !observation_service) {
		return _error("OBSERVATION_SERVICE_UNAVAILABLE", "Solers observation service is not initialized.", false);
	}
	if (editor_operator_tool && !editor_operator) {
		return _error("EDITOR_OPERATOR_UNAVAILABLE", "Solers editor operator is not initialized.", false);
	}
	if (script_tool && !script_service && p_name != StringName("script.read")) {
		return _error("SCRIPT_SERVICE_UNAVAILABLE", "Solers script service is not initialized.", false);
	}
	if (settings_tool && !settings_service) {
		return _error("SETTINGS_SERVICE_UNAVAILABLE", "Solers settings service is not initialized.", false);
	}
	if (resource_tool && !resource_service) {
		return _error("RESOURCE_SERVICE_UNAVAILABLE", "Solers resource service is not initialized.", false);
	}
	if (rpc_tool && !rpc_server) {
		return _error("RPC_SERVER_UNAVAILABLE", "Solers RPC server is not initialized.", false);
	}

	Dictionary result;
	if (p_name == StringName("project.get_info")) {
		result = _ok(observation_service->get_project_info());
	} else if (p_name == StringName("project.get_settings_summary")) {
		result = _ok(observation_service->get_project_settings_summary());
	} else if (p_name == StringName("project.list_files")) {
		const int max_files = p_args.get("max_files", 512);
		result = _ok(observation_service->list_project_files(max_files));
	} else if (p_name == StringName("project.search_files")) {
		const String query = p_args.get("query", String());
		const int max_files = p_args.get("max_files", 128);
		result = _ok(observation_service->search_project_files(query, max_files));
	} else if (p_name == StringName("project.read_file")) {
		const String path = p_args.get("path", String());
		const int max_bytes = p_args.get("max_bytes", 262144);
		result = _ok(observation_service->read_project_file(path, max_bytes));
	} else if (p_name == StringName("project.write_file")) {
		result = script_service->write_file(p_args);
	} else if (p_name == StringName("script.read")) {
		if (!observation_service) {
			result = _error("OBSERVATION_SERVICE_UNAVAILABLE", "Solers observation service is not initialized.", false);
		} else {
			const String path = p_args.get("path", String());
			const int max_bytes = p_args.get("max_bytes", 262144);
			result = _ok(observation_service->read_project_file(path, max_bytes));
		}
	} else if (p_name == StringName("script.write")) {
		result = script_service->write_file(p_args);
	} else if (p_name == StringName("script.patch")) {
		result = script_service->patch_file(p_args);
	} else if (p_name == StringName("script.create")) {
		Dictionary args = p_args;
		args["create"] = true;
		args["overwrite"] = false;
		result = script_service->write_file(args);
	} else if (p_name == StringName("script.validate")) {
		result = script_service->validate_script(p_args);
	} else if (p_name == StringName("script.open_in_editor")) {
		result = script_service->open_script(p_args);
	} else if (p_name == StringName("validation.validate_project_scripts")) {
		result = script_service->validate_project_scripts(p_args);
	} else if (p_name == StringName("validation.assert_no_errors")) {
		Dictionary scripts = script_service->validate_project_scripts(p_args);
		Dictionary scripts_data = scripts.get("data", Dictionary());
		Dictionary logs = observation_service ? observation_service->get_editor_logs((int)p_args.get("max_log_messages", 200)) : Dictionary();
		Dictionary counts = logs.get("counts", Dictionary());
		const int script_error_count = scripts_data.get("error_count", 0);
		const int log_error_count = counts.get("errors", 0);
		Dictionary data;
		data["valid"] = script_error_count == 0 && log_error_count == 0;
		data["script_validation"] = scripts;
		data["editor_log"] = logs;
		data["script_error_count"] = script_error_count;
		data["log_error_count"] = log_error_count;
		result = _ok(data);
	} else if (p_name == StringName("validation.read_editor_errors")) {
		Dictionary logs = observation_service ? observation_service->get_editor_logs((int)p_args.get("max_messages", 200)) : Dictionary();
		Array messages = logs.get("messages", Array());
		Array filtered;
		for (int i = 0; i < messages.size(); i++) {
			Dictionary item = messages[i];
			const int type = item.get("type", 0);
			if (type == 1 || type == 3) {
				filtered.push_back(item);
			}
		}
		Dictionary data;
		data["messages"] = filtered;
		data["count"] = filtered.size();
		data["counts"] = logs.get("counts", Dictionary());
		result = _ok(data);
	} else if (p_name == StringName("validation.run_scene_smoke")) {
		Dictionary scripts = script_service->validate_project_scripts(p_args);
		Dictionary scene = observation_service ? observation_service->get_scene_tree((int)p_args.get("max_scene_depth", 3), (int)p_args.get("max_children_per_node", 64)) : Dictionary();
		Dictionary runtime = observation_service ? observation_service->get_runtime_status() : Dictionary();
		Dictionary logs = observation_service ? observation_service->get_editor_logs((int)p_args.get("max_log_messages", 100)) : Dictionary();
		Dictionary scripts_data = scripts.get("data", Dictionary());
		Dictionary log_counts = logs.get("counts", Dictionary());
		const bool has_scene = scene.get("has_edited_scene", false);
		const int script_error_count = scripts_data.get("error_count", 0);
		const int log_error_count = log_counts.get("errors", 0);
		Dictionary data;
		data["passed"] = has_scene && script_error_count == 0 && log_error_count == 0;
		data["has_edited_scene"] = has_scene;
		data["script_validation"] = scripts;
		data["scene_tree"] = scene;
		data["runtime"] = runtime;
		data["editor_log"] = logs;
		result = _ok(data);
	} else if (p_name == StringName("resource.get_info")) {
		result = resource_service->get_resource_info(p_args);
	} else if (p_name == StringName("export.list_presets")) {
		result = resource_service->list_export_presets(p_args);
	} else if (p_name == StringName("export.validate_presets")) {
		result = resource_service->validate_export_presets(p_args);
	} else if (p_name == StringName("provider.get_config")) {
		result = settings_service->get_provider_config();
	} else if (p_name == StringName("provider.list_profiles")) {
		result = settings_service->list_provider_profiles();
	} else if (p_name == StringName("provider.validate_config")) {
		result = settings_service->validate_provider_config(p_args);
	} else if (p_name == StringName("provider.set_config")) {
		result = settings_service->set_provider_config(p_args);
	} else if (p_name == StringName("approvals.list_pending")) {
		Dictionary data;
		data["pending"] = permission_manager->list_pending_requests();
		data["count"] = permission_manager->get_pending_request_count();
		result = _ok(data);
	} else if (p_name == StringName("rpc.get_status")) {
		result = _ok(rpc_server->get_status());
	} else if (p_name == StringName("rpc.start")) {
		result = rpc_server->start(p_args);
	} else if (p_name == StringName("rpc.stop")) {
		result = rpc_server->stop();
	} else if (p_name == StringName("scene.get_open_scenes")) {
		const int max_depth = p_args.get("max_depth", 1);
		const int max_children = p_args.get("max_children_per_node", 16);
		result = _ok(observation_service->get_open_scenes(max_depth, max_children));
	} else if (p_name == StringName("scene.open")) {
		result = editor_operator->open_scene(p_args);
	} else if (p_name == StringName("scene.create")) {
		result = editor_operator->create_scene(p_args);
	} else if (p_name == StringName("scene.get_tree")) {
		const int max_depth = p_args.get("max_depth", 8);
		const int max_children = p_args.get("max_children_per_node", 128);
		result = _ok(observation_service->get_scene_tree(max_depth, max_children));
	} else if (p_name == StringName("selection.get_nodes")) {
		const int max_depth = p_args.get("max_depth", 1);
		const int max_children = p_args.get("max_children_per_node", 16);
		result = _ok(observation_service->get_selection(max_depth, max_children));
	} else if (p_name == StringName("node.get_properties")) {
		result = editor_operator->get_node_properties(p_args);
	} else if (p_name == StringName("runtime.get_status")) {
		result = _ok(observation_service->get_runtime_status());
	} else if (p_name == StringName("runtime.get_logs") || p_name == StringName("editor.get_logs")) {
		result = _ok(observation_service->get_editor_logs((int)p_args.get("max_messages", 200)));
	} else if (p_name == StringName("runtime.capture_screenshot") || p_name == StringName("editor.capture_screenshot")) {
		result = editor_operator->capture_editor_screenshot(p_args);
	} else if (p_name == StringName("editor.get_snapshot")) {
		const int max_depth = p_args.get("max_scene_depth", 4);
		const int max_children = p_args.get("max_children_per_node", 64);
		result = _ok(observation_service->get_editor_snapshot(max_depth, max_children));
	} else if (p_name == StringName("timeline.list_actions")) {
		const int limit = p_args.get("limit", 100);
		result = _ok(action_timeline ? action_timeline->list_actions(limit) : Array());
	} else if (p_name == StringName("timeline.rollback_last")) {
		result = editor_operator->rollback_last_editor_action(p_args);
	} else if (p_name == StringName("node.add")) {
		result = editor_operator->add_node(p_args);
	} else if (p_name == StringName("node.reparent")) {
		result = editor_operator->reparent_node(p_args);
	} else if (p_name == StringName("node.attach_script")) {
		result = editor_operator->attach_script(p_args);
	} else if (p_name == StringName("node.connect_signal")) {
		result = editor_operator->connect_signal(p_args);
	} else if (p_name == StringName("node.list_signal_connections")) {
		result = editor_operator->list_signal_connections(p_args);
	} else if (p_name == StringName("node.set_properties")) {
		result = editor_operator->set_node_properties(p_args);
	} else if (p_name == StringName("node.remove")) {
		result = editor_operator->remove_node(p_args);
	} else if (p_name == StringName("scene.save")) {
		result = editor_operator->save_current_scene(p_args);
	} else if (p_name == StringName("scene.save_as")) {
		result = editor_operator->save_scene_as(p_args);
	} else if (p_name == StringName("runtime.play_current_scene")) {
		result = editor_operator->play_current_scene(p_args);
	} else if (p_name == StringName("runtime.stop")) {
		result = editor_operator->stop_playing_scene(p_args);
	} else {
		result = _error("TOOL_HANDLER_NOT_FOUND", vformat("Solers tool has no handler: %s", p_name), false);
	}

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
