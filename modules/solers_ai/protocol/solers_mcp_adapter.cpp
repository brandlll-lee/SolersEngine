/**************************************************************************/
/*  solers_mcp_adapter.cpp                                                */
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

#include "solers_mcp_adapter.h"

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include "modules/solers_ai/core/solers_project_observation.h"
#include "modules/solers_ai/core/solers_runtime_observation.h"
#include "modules/solers_ai/core/solers_scene_observation.h"
#include "modules/solers_ai/core/solers_tool_executor.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersMCPAdapter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tool_registry", "tool_registry"), &SolersMCPAdapter::set_tool_registry);
	ClassDB::bind_method(D_METHOD("handle_request", "request"), &SolersMCPAdapter::handle_request);
	ClassDB::bind_method(D_METHOD("begin_request", "request"), &SolersMCPAdapter::begin_request);
	ClassDB::bind_method(D_METHOD("poll"), &SolersMCPAdapter::poll);
	ClassDB::bind_method(D_METHOD("take_response", "request_token"), &SolersMCPAdapter::take_response);
	ClassDB::bind_method(D_METHOD("cancel_request", "request_token"), &SolersMCPAdapter::cancel_request);
	ClassDB::bind_method(D_METHOD("initialize", "params"), &SolersMCPAdapter::initialize);
	ClassDB::bind_method(D_METHOD("list_tools"), &SolersMCPAdapter::list_tools);
	ClassDB::bind_method(D_METHOD("call_tool", "params"), &SolersMCPAdapter::call_tool);
	ClassDB::bind_method(D_METHOD("list_resources"), &SolersMCPAdapter::list_resources);
	ClassDB::bind_method(D_METHOD("read_resource", "params"), &SolersMCPAdapter::read_resource);
	ClassDB::bind_method(D_METHOD("list_prompts"), &SolersMCPAdapter::list_prompts);
	ClassDB::bind_method(D_METHOD("get_status"), &SolersMCPAdapter::get_status);
}

Dictionary SolersMCPAdapter::_jsonrpc_result(const Variant &p_id, const Variant &p_result) const {
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["result"] = p_result;
	return response;
}

Dictionary SolersMCPAdapter::_jsonrpc_error(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data) const {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	if (p_data.get_type() != Variant::NIL) {
		error["data"] = p_data;
	}

	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["error"] = error;
	return response;
}

Dictionary SolersMCPAdapter::_content_text(const String &p_text) const {
	Dictionary content;
	content["type"] = "text";
	content["text"] = p_text;
	return content;
}

Dictionary SolersMCPAdapter::_resource(const String &p_uri, const String &p_name, const String &p_description, const String &p_mime_type) const {
	Dictionary resource;
	resource["uri"] = p_uri;
	resource["name"] = p_name;
	resource["description"] = p_description;
	resource["mimeType"] = p_mime_type;
	return resource;
}

Array SolersMCPAdapter::_tool_definitions_for_mcp() const {
	Array tools;
	Array definitions = tool_registry ? tool_registry->list_tools() : Array();
	for (int i = 0; i < definitions.size(); i++) {
		Dictionary definition = definitions[i];
		Dictionary tool;
		tool["name"] = definition.get("name", String());
		tool["description"] = definition.get("description", String());
		tool["inputSchema"] = definition.get("input_schema", Dictionary());
		if (definition.has("output_schema")) {
			tool["outputSchema"] = definition["output_schema"];
		}

		tools.push_back(tool);
	}
	return tools;
}

void SolersMCPAdapter::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
}

void SolersMCPAdapter::set_tool_executor(SolersToolExecutor *p_tool_executor) {
	tool_executor = p_tool_executor;
}

void SolersMCPAdapter::set_observations(SolersProjectObservation *p_project, SolersSceneObservation *p_scene, SolersRuntimeObservation *p_runtime) {
	project_observation = p_project;
	scene_observation = p_scene;
	runtime_observation = p_runtime;
}

Dictionary SolersMCPAdapter::begin_request(const Dictionary &p_request) {
	const String method = p_request.get("method", String());
	if (method != "tools/call") {
		return Dictionary({ { "deferred", false }, { "response", handle_request(p_request) } });
	}
	const Variant params_value = p_request.get("params", Variant());
	if (params_value.get_type() != Variant::DICTIONARY) {
		return Dictionary({ { "deferred", false }, { "response", _jsonrpc_error(p_request.get("id", Variant()), -32602, "tools/call params must be an object.") } });
	}
	const Dictionary params = params_value;
	const Variant arguments_value = params.get("arguments", Dictionary());
	if (String(params.get("name", String())).is_empty() || arguments_value.get_type() != Variant::DICTIONARY) {
		return Dictionary({ { "deferred", false }, { "response", _jsonrpc_error(p_request.get("id", Variant()), -32602, "tools/call requires a non-empty name and object arguments.") } });
	}
	const int64_t request_token = next_request_token++;
	Dictionary pending;
	pending["request_token"] = request_token;
	pending["rpc_id"] = p_request.get("id", Variant());
	pending["name"] = params.get("name", String());
	pending["arguments"] = Dictionary(arguments_value).duplicate(true);
	pending["call_id"] = vformat("mcp-%d", request_token);
	pending_tool_requests.push_back(pending);
	return Dictionary({ { "deferred", true }, { "request_token", request_token } });
}

void SolersMCPAdapter::poll() {
	if (!tool_executor) {
		return;
	}
	if (active_tool_request.is_empty()) {
		if (pending_tool_requests.is_empty() || !tool_executor->is_idle()) {
			return;
		}
		active_tool_request = pending_tool_requests[0];
		pending_tool_requests.remove_at(0);
		SolersToolContext context;
		context.call_id = active_tool_request.get("call_id", String());
		context.session_id = "mcp";
		context.project_path = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->get_resource_path() : String();
		const Dictionary arguments = active_tool_request.get("arguments", Dictionary());
		const int requested_timeout = (int)arguments.get("timeout_msec", 600000);
		const Dictionary start_error = tool_executor->start(StringName(active_tool_request.get("name", String())), arguments, context, MIN((uint64_t)MAX(requested_timeout, 1), (uint64_t)600000));
		if (!start_error.is_empty()) {
			Dictionary response;
			response["structuredContent"] = start_error;
			response["isError"] = true;
			response["content"] = Array({ _content_text(JSON::stringify(start_error, "\t", false, true)) });
			completed_responses[(int64_t)active_tool_request["request_token"]] = _jsonrpc_result(active_tool_request.get("rpc_id", Variant()), response);
			active_tool_request.clear();
			return;
		}
	}

	const String call_id = active_tool_request.get("call_id", String());
	if (tool_executor->get_call_id() != call_id) {
		return;
	}
	tool_executor->poll();
	if (!tool_executor->is_terminal()) {
		return;
	}
	const Dictionary tool_result = tool_executor->take_result();
	Dictionary response;
	response["structuredContent"] = tool_result;
	response["isError"] = !(bool)tool_result.get("ok", false);
	response["content"] = Array({ _content_text(JSON::stringify(tool_result, "\t", false, true)) });
	completed_responses[(int64_t)active_tool_request["request_token"]] = _jsonrpc_result(active_tool_request.get("rpc_id", Variant()), response);
	active_tool_request.clear();
}

Dictionary SolersMCPAdapter::take_response(int64_t p_request_token) {
	const Dictionary *response = completed_responses.getptr(p_request_token);
	if (!response) {
		return Dictionary();
	}
	const Dictionary result = *response;
	completed_responses.erase(p_request_token);
	return result;
}

bool SolersMCPAdapter::cancel_request(int64_t p_request_token) {
	for (int i = 0; i < pending_tool_requests.size(); i++) {
		if ((int64_t)Dictionary(pending_tool_requests[i]).get("request_token", 0) == p_request_token) {
			pending_tool_requests.remove_at(i);
			return true;
		}
	}
	if (!active_tool_request.is_empty() && (int64_t)active_tool_request.get("request_token", 0) == p_request_token) {
		if (tool_executor && tool_executor->get_call_id() == String(active_tool_request.get("call_id", String())) && tool_executor->is_active()) {
			tool_executor->cancel("TOOL_CANCELLED", "The MCP client disconnected before the tool reached a terminal state.");
		}
		if (tool_executor && tool_executor->is_terminal() && tool_executor->get_call_id() == String(active_tool_request.get("call_id", String()))) {
			tool_executor->take_result();
		}
		active_tool_request.clear();
		return true;
	}
	if (completed_responses.has(p_request_token)) {
		completed_responses.erase(p_request_token);
		return true;
	}
	return false;
}

Dictionary SolersMCPAdapter::handle_request(const Dictionary &p_request) {
	const Variant id = p_request.get("id", Variant());
	const String method = p_request.get("method", String());
	const Dictionary params = p_request.get("params", Dictionary());

	Dictionary result;
	if (method == "initialize") {
		result = initialize(params);
	} else if (method == "tools/list") {
		result = list_tools();
	} else if (method == "tools/call") {
		result = call_tool(params);
	} else if (method == "resources/list") {
		result = list_resources();
	} else if (method == "resources/read") {
		result = read_resource(params);
	} else if (method == "prompts/list") {
		result = list_prompts();
	} else if (method == "solers/status") {
		result = get_status();
	} else if (method == "ping") {
		Dictionary pong;
		pong["status"] = "ok";
		result = pong;
	} else {
		return _jsonrpc_error(id, -32601, vformat("Method not found: %s", method));
	}

	return _jsonrpc_result(id, result);
}

Dictionary SolersMCPAdapter::initialize(const Dictionary &p_params) const {
	Dictionary result;
	result["protocolVersion"] = p_params.get("protocolVersion", "2025-11-25");

	Dictionary server_info;
	server_info["name"] = "solers-engine";
	server_info["version"] = "0.1.0";
	result["serverInfo"] = server_info;

	Dictionary capabilities;
	capabilities["tools"] = Dictionary();
	capabilities["resources"] = Dictionary();
	capabilities["prompts"] = Dictionary();
	result["capabilities"] = capabilities;
	return result;
}

Dictionary SolersMCPAdapter::list_tools() const {
	Dictionary result;
	result["tools"] = _tool_definitions_for_mcp();
	return result;
}

Dictionary SolersMCPAdapter::call_tool(const Dictionary &p_params) {
	const Dictionary dispatch = begin_request(Dictionary({ { "jsonrpc", "2.0" }, { "id", next_request_token }, { "method", "tools/call" }, { "params", p_params } }));
	if (!(bool)dispatch.get("deferred", false)) {
		return dispatch.get("response", Dictionary());
	}
	return Dictionary({ { "status", "accepted" }, { "request_token", dispatch.get("request_token", 0) } });
}

Dictionary SolersMCPAdapter::list_resources() const {
	Array resources;
	resources.push_back(_resource("solers://editor/snapshot", "Editor Snapshot", "Current project, scene, selection, and runtime snapshot."));

	Dictionary result;
	result["resources"] = resources;
	return result;
}

Dictionary SolersMCPAdapter::read_resource(const Dictionary &p_params) const {
	const String uri = p_params.get("uri", String());
	Dictionary result;
	Array contents;
	Dictionary item;
	item["uri"] = uri;
	item["mimeType"] = "application/json";

	Variant data;
	if (uri == "solers://editor/snapshot") {
		Dictionary snapshot;
		if (project_observation) {
			snapshot["project"] = project_observation->get_project_info();
			snapshot["project_settings"] = project_observation->get_project_settings_summary();
		}
		if (scene_observation) {
			snapshot["open_scenes"] = scene_observation->get_open_scenes();
			snapshot["selection"] = scene_observation->get_selection();
		}
		if (runtime_observation) {
			snapshot["runtime"] = runtime_observation->get_runtime_status();
		}
		data = snapshot;
	} else {
		Dictionary error;
		error["ok"] = false;
		error["error"] = "Unknown Solers resource URI.";
		data = error;
	}

	item["text"] = JSON::stringify(data, "\t", false, true);
	contents.push_back(item);
	result["contents"] = contents;
	return result;
}

Dictionary SolersMCPAdapter::list_prompts() const {
	Array prompts;

	Dictionary create_game;
	create_game["name"] = "solers_create_playable_prototype";
	create_game["description"] = "Plan, build, run, and verify a small playable Godot-compatible prototype through Solers tools.";
	prompts.push_back(create_game);

	Dictionary debug_scene;
	debug_scene["name"] = "solers_debug_current_scene";
	debug_scene["description"] = "Inspect the current scene, run it, read errors, and propose a repair tool batch.";
	prompts.push_back(debug_scene);

	Dictionary result;
	result["prompts"] = prompts;
	return result;
}

Dictionary SolersMCPAdapter::get_status() const {
	Dictionary result;
	result["tools_available"] = tool_registry ? tool_registry->get_tool_count() : 0;
	return result;
}
