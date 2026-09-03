/**************************************************************************/
/*  solers_mcp_adapter.h                                                  */
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

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

class SolersProjectObservation;
class SolersRuntimeObservation;
class SolersSceneObservation;
class SolersToolExecutor;
class SolersToolRegistry;

class SolersMCPAdapter : public Object {
	GDCLASS(SolersMCPAdapter, Object);

	SolersToolRegistry *tool_registry = nullptr;
	SolersToolExecutor *tool_executor = nullptr;
	SolersProjectObservation *project_observation = nullptr;
	SolersRuntimeObservation *runtime_observation = nullptr;
	SolersSceneObservation *scene_observation = nullptr;
	Array pending_tool_requests;
	Dictionary active_tool_request;
	HashMap<int64_t, Dictionary> completed_responses;
	int64_t next_request_token = 1;

	Dictionary _jsonrpc_result(const Variant &p_id, const Variant &p_result) const;
	Dictionary _jsonrpc_error(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data = Variant()) const;
	Dictionary _content_text(const String &p_text) const;
	Array _tool_definitions_for_mcp() const;
	Dictionary _resource(const String &p_uri, const String &p_name, const String &p_description, const String &p_mime_type = "application/json") const;

protected:
	static void _bind_methods();

public:
	void set_tool_registry(SolersToolRegistry *p_tool_registry);
	void set_tool_executor(SolersToolExecutor *p_tool_executor);
	void set_observations(SolersProjectObservation *p_project, SolersSceneObservation *p_scene, SolersRuntimeObservation *p_runtime);

	Dictionary handle_request(const Dictionary &p_request);
	Dictionary begin_request(const Dictionary &p_request);
	void poll();
	Dictionary take_response(int64_t p_request_token);
	bool cancel_request(int64_t p_request_token);
	Dictionary initialize(const Dictionary &p_params) const;
	Dictionary list_tools() const;
	Dictionary call_tool(const Dictionary &p_params);
	Dictionary list_resources() const;
	Dictionary read_resource(const Dictionary &p_params) const;
	Dictionary list_prompts() const;
	Dictionary get_status() const;
};
