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

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "modules/solers_ai/core/solers_action_timeline.h"
#include "modules/solers_ai/core/solers_observation_service.h"
#include "modules/solers_ai/core/solers_tool_registry.h"

void SolersMCPAdapter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tool_registry", "tool_registry"), &SolersMCPAdapter::set_tool_registry);
	ClassDB::bind_method(D_METHOD("set_observation_service", "observation_service"), &SolersMCPAdapter::set_observation_service);
	ClassDB::bind_method(D_METHOD("set_action_timeline", "action_timeline"), &SolersMCPAdapter::set_action_timeline);
	ClassDB::bind_method(D_METHOD("handle_request", "request"), &SolersMCPAdapter::handle_request);
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
		const String exposure = definition.get("exposure", "direct");
		if (exposure == "deferred" || exposure == "hidden") {
			continue;
		}
		Dictionary tool;
		tool["name"] = definition.get("name", String());
		tool["description"] = definition.get("description", String());
		tool["inputSchema"] = definition.get("input_schema", Dictionary());
		if (definition.has("output_schema")) {
			tool["outputSchema"] = definition["output_schema"];
		}

		Dictionary annotations;
		annotations["title"] = definition.get("name", String());
		annotations["modelName"] = definition.get("model_name", String());
		annotations["readOnlyHint"] = String(definition.get("mutation_kind", "none")) == "none";
		annotations["destructiveHint"] = (bool)definition.get("requires_approval", false);
		annotations["openWorldHint"] = false;
		tool["annotations"] = annotations;
		tools.push_back(tool);
	}
	return tools;
}

void SolersMCPAdapter::set_tool_registry(SolersToolRegistry *p_tool_registry) {
	tool_registry = p_tool_registry;
}

void SolersMCPAdapter::set_observation_service(SolersObservationService *p_observation_service) {
	observation_service = p_observation_service;
}

void SolersMCPAdapter::set_action_timeline(SolersActionTimeline *p_action_timeline) {
	action_timeline = p_action_timeline;
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
	Dictionary result;
	if (!tool_registry) {
		result["isError"] = true;
		Array content;
		content.push_back(_content_text("SolersToolRegistry is unavailable."));
		result["content"] = content;
		return result;
	}

	const StringName name = StringName(p_params.get("name", String()));
	const Dictionary arguments = p_params.get("arguments", Dictionary());
	Dictionary tool_result = tool_registry->call_tool(name, arguments);

	result["structuredContent"] = tool_result;
	result["isError"] = !(bool)tool_result.get("ok", false);
	Array content;
	content.push_back(_content_text(JSON::stringify(tool_result, "\t", false, true)));
	result["content"] = content;
	return result;
}

Dictionary SolersMCPAdapter::list_resources() const {
	Array resources;
	resources.push_back(_resource("solers://editor/snapshot", "Editor Snapshot", "Current project, scene, selection, and runtime snapshot."));
	resources.push_back(_resource("solers://timeline/actions", "Action Timeline", "Recent Solers action timeline events."));

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
		data = observation_service ? observation_service->get_editor_snapshot(4, 64) : Dictionary();
	} else if (uri == "solers://timeline/actions") {
		data = action_timeline ? action_timeline->list_actions(100) : Array();
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
	result["timeline_events"] = action_timeline ? action_timeline->get_action_count() : 0;
	return result;
}
